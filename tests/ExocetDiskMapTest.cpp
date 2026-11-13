#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/core/ExocetDiskMap.h"

using adm::core::ExocetDiskMap;
using adm::core::ExocetDiskSpec;
using adm::core::ParseExocetDiskSpec;
using adm::core::ReadExocetDiskMap;
using ojson = nlohmann::ordered_json;

namespace {

// The shape of the crass anim production's "disk" section (productions/anim/sync/*.exocet), cut
// down: a magic, a version, a count and the player's place in the header; the five entry roles.
ojson Spec()
{
    return ojson::parse(R"({
      "name": "XCET",
      "text": "a test disk",
      "sector_bytes": 512,
      "sectors": 1760,
      "sectors_per_track": 11,
      "regions": [
        {"name": "bootblock", "first": 0, "count": 2},
        {"name": "directory", "first": 2, "count": 5},
        {"name": "logo", "first": 7, "end": "player_sector"},
        {"name": "player", "first": "player_sector", "count": "player_sectors"}
      ],
      "directory": {
        "sector": 2, "sectors": 5, "magic": "XCET", "version": 14, "count": "blocks",
        "header": [
          {"name": "magic", "type": "char4"},
          {"name": "version", "type": "u16"},
          {"name": "blocks", "type": "u16", "unit": "count"},
          {"name": "largest", "type": "u32", "unit": "bytes"},
          {"name": "player_sector", "type": "u32", "unit": "sector"},
          {"name": "player_sectors", "type": "u32", "unit": "sectors"}
        ],
        "entry": [
          {"name": "sector", "type": "u32", "role": "sector"},
          {"name": "packed", "type": "u32", "role": "packed"},
          {"name": "unpacked", "type": "u32", "role": "unpacked"},
          {"name": "kind", "type": "u16", "role": "kind"},
          {"name": "sectors", "type": "u16", "role": "sectors"}
        ]
      },
      "kinds": [
        {"value": 1, "name": "anim", "text": "frames"},
        {"value": 7, "name": "music", "text": "the module"}
      ]
    })");
}

void Put16(std::vector<uint8_t>& d, size_t at, uint32_t v)
{
    d[at] = uint8_t(v >> 8);
    d[at + 1] = uint8_t(v);
}

void Put32(std::vector<uint8_t>& d, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        d[at + size_t(i)] = uint8_t(v >> (24 - 8 * i));
}

struct Block
{
    uint32_t packed, unpacked;
    uint16_t kind;
};

// An ADF as disk_kit.write_disk lays one out: the blocks from sector `first`, each from a fresh
// sector, the header and the entries at sector 2.
std::vector<uint8_t> Disk(const std::vector<Block>& blocks, uint32_t first = 88,
                          const char* magic = "XCET", uint16_t version = 14)
{
    std::vector<uint8_t> d(1760 * 512, 0);
    const size_t dir = 2 * 512;
    for (int i = 0; i < 4; ++i)
        d[dir + size_t(i)] = uint8_t(magic[i]);
    Put16(d, dir + 4, version);
    Put16(d, dir + 6, uint16_t(blocks.size()));
    Put32(d, dir + 8, 6144);
    Put32(d, dir + 12, 39); // the player
    Put32(d, dir + 16, 49);
    uint32_t at = first;
    size_t e = dir + 20;
    for (const auto& b : blocks) {
        const uint32_t n = (b.packed + 511) / 512;
        Put32(d, e, at);
        Put32(d, e + 4, b.packed);
        Put32(d, e + 8, b.unpacked);
        Put16(d, e + 12, b.kind);
        Put16(d, e + 14, uint16_t(n));
        e += 16;
        at += n;
    }
    return d;
}

ExocetDiskSpec Parsed(const ojson& j = Spec())
{
    ExocetDiskSpec s;
    std::string error;
    EXPECT_TRUE(ParseExocetDiskSpec(j, &s, &error)) << error;
    return s;
}

} // namespace

TEST(ExocetDiskMap, ParsesTheSpec)
{
    const ExocetDiskSpec s = Parsed();
    EXPECT_EQ(s.name, "XCET");
    EXPECT_EQ(s.dirSector, 2u);
    EXPECT_EQ(s.dirSectors, 5u);
    EXPECT_EQ(s.headerBytes, 20u);
    EXPECT_EQ(s.entryBytes, 16u);
    ASSERT_EQ(s.header.size(), 6u);
    EXPECT_TRUE(s.header[0].isText);
    EXPECT_EQ(s.header[3].offset, 8u);
    ASSERT_NE(s.FindKind(7), nullptr);
    EXPECT_EQ(s.FindKind(7)->name, "music");
    EXPECT_EQ(s.FindKind(3), nullptr);
}

TEST(ExocetDiskMap, RefusesAMapItCannotRead)
{
    ExocetDiskSpec s;
    std::string error;
    ojson j = Spec();
    j["directory"]["count"] = "magic"; // a text field
    EXPECT_FALSE(ParseExocetDiskSpec(j, &s, &error));
    j = Spec();
    j["directory"]["entry"][3]["role"] = "";
    EXPECT_FALSE(ParseExocetDiskSpec(j, &s, &error));
    EXPECT_NE(error.find("kind"), std::string::npos);
    j = Spec();
    j["directory"]["header"][1]["type"] = "u24";
    EXPECT_FALSE(ParseExocetDiskSpec(j, &s, &error));
    j = Spec();
    j["regions"][0].erase("count");
    EXPECT_FALSE(ParseExocetDiskSpec(j, &s, &error));
}

TEST(ExocetDiskMap, ReadsTheHeaderEntriesRunsAndKinds)
{
    const ExocetDiskSpec s = Parsed();
    // two anim blocks straight on, the music, then anim again: two anim runs
    const ExocetDiskMap m = ReadExocetDiskMap(
        s, Disk({{6144, 10000, 1}, {1000, 3000, 1}, {3072, 3072, 7}, {512, 700, 1}}));
    ASSERT_TRUE(m.ok) << m.error;
    EXPECT_TRUE(m.warnings.empty());
    ASSERT_EQ(m.header.size(), 6u);
    EXPECT_EQ(m.header[0].text, "XCET");
    EXPECT_EQ(m.header[4].value, 39u);

    ASSERT_EQ(m.entries.size(), 4u);
    EXPECT_EQ(m.entries[0].sector, 88u);
    EXPECT_EQ(m.entries[0].sectors, 12u);
    EXPECT_EQ(m.entries[1].sector, 100u);
    EXPECT_EQ(m.entries[1].streamOffset, 10000u);
    EXPECT_EQ(m.entries[2].kindName, "music");
    EXPECT_EQ(m.entries[3].streamOffset, 13000u);
    EXPECT_EQ(m.usedEnd, 109u);

    ASSERT_EQ(m.runs.size(), 3u);
    EXPECT_EQ(m.runs[0].firstEntry, 0);
    EXPECT_EQ(m.runs[0].lastEntry, 1);
    EXPECT_EQ(m.runs[0].firstSector, 88u);
    EXPECT_EQ(m.runs[0].endSector, 102u);
    EXPECT_EQ(m.runs[0].unpacked, 13000u);
    EXPECT_EQ(m.runs[2].kindName, "anim");

    ASSERT_EQ(m.kinds.size(), 2u);
    EXPECT_EQ(m.kinds[0].kind, 1);
    EXPECT_EQ(m.kinds[0].blocks, 3);
    EXPECT_EQ(m.kinds[0].packed, 7656u);
}

TEST(ExocetDiskMap, WorksOutTheRegions)
{
    const ExocetDiskMap m = ReadExocetDiskMap(Parsed(), Disk({{1024, 2048, 1}}));
    ASSERT_TRUE(m.ok) << m.error;
    // bootblock, directory, logo (7..38, by the player's field), player, blocks, unused
    ASSERT_EQ(m.regions.size(), 6u);
    EXPECT_EQ(m.regions[2].name, "logo");
    EXPECT_EQ(m.regions[2].first, 7u);
    EXPECT_EQ(m.regions[2].count, 32u);
    EXPECT_EQ(m.regions[3].name, "player");
    EXPECT_EQ(m.regions[3].count, 49u);
    EXPECT_EQ(m.regions[4].name, "blocks");
    EXPECT_TRUE(m.regions[4].fromEntries);
    EXPECT_EQ(m.regions[5].name, "unused");
    EXPECT_EQ(m.regions[5].first, 90u);
    EXPECT_EQ(m.regions[5].count, 1670u);
    EXPECT_TRUE(m.warnings.empty());
}

TEST(ExocetDiskMap, KnowsAnotherDisk)
{
    const ExocetDiskMap m = ReadExocetDiskMap(Parsed(), Disk({{512, 512, 1}}, 88, "SNOD"));
    EXPECT_FALSE(m.ok);
    EXPECT_NE(m.error.find("SNOD"), std::string::npos);
    EXPECT_FALSE(ReadExocetDiskMap(Parsed(), std::vector<uint8_t>(1000)).ok);
}

TEST(ExocetDiskMap, WarnsOfAnotherVersionAndABadEntry)
{
    auto d = Disk({{512, 512, 1}, {600, 900, 9}}, 88, "XCET", 13);
    Put16(d, 2 * 512 + 20 + 16 + 14, 1); // 600 bytes in one sector
    const ExocetDiskMap m = ReadExocetDiskMap(Parsed(), d);
    ASSERT_TRUE(m.ok) << m.error;
    ASSERT_EQ(m.warnings.size(), 2u);
    EXPECT_NE(m.warnings[0].find("version 13"), std::string::npos);
    EXPECT_FALSE(m.entries[1].fits);
    EXPECT_EQ(m.entries[1].kindName, "kind 9");
}

TEST(ExocetDiskMap, RefusesMoreEntriesThanTheDirectoryHolds)
{
    auto d = Disk({{512, 512, 1}});
    Put16(d, 2 * 512 + 6, 200); // 5 sectors hold 158 after a 20-byte header
    const ExocetDiskMap m = ReadExocetDiskMap(Parsed(), d);
    EXPECT_FALSE(m.ok);
    EXPECT_NE(m.error.find("200"), std::string::npos);
}

namespace {

// a slice as the crass tools write one: packed size (0x8000 for ZX0), unpacked size, the bytes,
// padded to even
void Slice(std::vector<uint8_t>& b, uint16_t bytes, uint16_t raw, bool zx0)
{
    const uint16_t size = zx0 ? uint16_t(bytes | 0x8000) : bytes;
    b.push_back(uint8_t(size >> 8));
    b.push_back(uint8_t(size));
    b.push_back(uint8_t(raw >> 8));
    b.push_back(uint8_t(raw));
    b.insert(b.end(), bytes, 0xAA);
    if (b.size() & 1)
        b.push_back(0);
}

ExocetDiskSpec SliceSpec()
{
    ojson j = Spec();
    j["slices"] = {{"zx0_flag", 32768}};
    j["kinds"][0]["packer"] = "zx0";
    j["kinds"][1]["packer"] = "none";
    return Parsed(j);
}

} // namespace

TEST(ExocetDiskMap, ReadsABlocksPacker)
{
    const ExocetDiskSpec s = SliceSpec();
    int n = 0;
    std::vector<uint8_t> b;
    Slice(b, 301, 2048, true);
    EXPECT_EQ(adm::core::ExocetDetectPacker(s, b.data(), uint32_t(b.size()), 2048, &n), "zx0");
    EXPECT_EQ(n, 1);

    b.clear();
    Slice(b, 1000, 2048, false);
    Slice(b, 777, 1500, false);
    EXPECT_EQ(adm::core::ExocetDetectPacker(s, b.data(), uint32_t(b.size()), 3548, &n), "lz4");
    EXPECT_EQ(n, 2);

    b.clear();
    Slice(b, 500, 2048, false);
    Slice(b, 400, 2048, true);
    EXPECT_EQ(adm::core::ExocetDetectPacker(s, b.data(), uint32_t(b.size()), 4096), "mixed");

    // the unpacked sizes must add up to the entry's, and raw data is none
    EXPECT_EQ(adm::core::ExocetDetectPacker(s, b.data(), uint32_t(b.size()), 4000), "?");
    const std::vector<uint8_t> raw(512, 0x55);
    EXPECT_EQ(adm::core::ExocetDetectPacker(s, raw.data(), 512, 512), "none");
}

TEST(ExocetDiskMap, RefusesAPackerItDoesNotKnow)
{
    ojson j = Spec();
    j["kinds"][0]["packer"] = "shrinkler";
    ExocetDiskSpec s;
    std::string error;
    EXPECT_FALSE(ParseExocetDiskSpec(j, &s, &error));
    j = Spec();
    j["slices"] = {{"zx0_flag", 3}};
    EXPECT_FALSE(ParseExocetDiskSpec(j, &s, &error));
}

TEST(ExocetDiskMap, MarksAPackerTheMapDoesNotSay)
{
    const ExocetDiskSpec s = SliceSpec();
    // anim (map: zx0): a ZX0 block, then an LZ4 one; music (map: none): raw
    auto d = Disk({{0, 2048, 1}, {0, 2048, 1}, {512, 512, 7}});
    std::vector<uint8_t> a, b;
    Slice(a, 700, 2048, true);
    Slice(b, 900, 2048, false);
    const size_t dir = 2 * 512 + 20;
    Put32(d, dir + 4, uint32_t(a.size()));
    Put16(d, dir + 14, uint16_t((a.size() + 511) / 512));
    Put32(d, dir + 16, 90); // the second after the first's two sectors
    Put32(d, dir + 16 + 4, uint32_t(b.size()));
    Put16(d, dir + 16 + 14, uint16_t((b.size() + 511) / 512));
    Put32(d, dir + 32, 92);
    std::copy(a.begin(), a.end(), d.begin() + 88 * 512);
    std::copy(b.begin(), b.end(), d.begin() + 90 * 512);

    const ExocetDiskMap m = ReadExocetDiskMap(s, d);
    ASSERT_TRUE(m.ok) << m.error;
    ASSERT_EQ(m.entries.size(), 3u);
    EXPECT_EQ(m.entries[0].packer, "zx0");
    EXPECT_TRUE(m.entries[0].packerOk);
    EXPECT_EQ(m.entries[1].packer, "lz4");
    EXPECT_FALSE(m.entries[1].packerOk);
    EXPECT_EQ(m.entries[2].packer, "none");
    EXPECT_TRUE(m.entries[2].packerOk);
    ASSERT_EQ(m.runs.size(), 2u);
    EXPECT_EQ(m.runs[0].packer, "mixed");
    EXPECT_FALSE(m.runs[0].packerOk);
    ASSERT_EQ(m.kinds.size(), 2u);
    EXPECT_EQ(m.kinds[0].packer, "zx0");
    EXPECT_EQ(m.kinds[0].found, "mixed");
    ASSERT_EQ(m.warnings.size(), 1u);
    EXPECT_NE(m.warnings[0].find("1 of its 2 blocks"), std::string::npos);
}
