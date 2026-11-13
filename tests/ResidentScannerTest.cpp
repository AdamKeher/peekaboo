#include <gtest/gtest.h>

#include <map>
#include <string>

#include "../src/core/ResidentScanner.h"

using adm::core::ResidentModule;
using adm::core::ResidentScanner;

namespace ex = adm::core::exec;

namespace {

class FakeMem
{
public:
    void PutByte(uint32_t a, uint8_t v) { m_bytes[a] = v; }
    void PutWord(uint32_t a, uint16_t v)
    {
        PutByte(a, static_cast<uint8_t>(v >> 8));
        PutByte(a + 1, static_cast<uint8_t>(v));
    }
    void PutLong(uint32_t a, uint32_t v)
    {
        PutWord(a, static_cast<uint16_t>(v >> 16));
        PutWord(a + 2, static_cast<uint16_t>(v));
    }
    void PutCString(uint32_t a, const std::string& s)
    {
        for (size_t i = 0; i < s.size(); ++i)
            PutByte(a + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
        PutByte(a + static_cast<uint32_t>(s.size()), 0);
    }
    void PutEmptyList(uint32_t a)
    {
        PutLong(a, a + 4);
        PutLong(a + 4, 0);
    }
    void PutList(uint32_t listAddr, const std::vector<uint32_t>& nodes)
    {
        PutLong(listAddr, nodes.empty() ? listAddr + 4 : nodes.front());
        for (size_t i = 0; i + 1 < nodes.size(); ++i)
            PutLong(nodes[i], nodes[i + 1]);
        if (!nodes.empty())
            PutLong(nodes.back(), listAddr + 4);
        PutLong(listAddr + 4, 0);
    }

    // Writes a valid struct Resident at `addr`.
    void PutResident(uint32_t addr, const std::string& name, uint8_t type, int8_t pri,
                     uint32_t nameAddr)
    {
        PutWord(addr + ex::RTAG_MATCHWORD, ex::RTAG_MATCHWORD_VALUE);
        PutLong(addr + ex::RTAG_MATCHTAG, addr);
        PutLong(addr + ex::RTAG_ENDSKIP, addr + 0x100);
        PutByte(addr + ex::RTAG_FLAGS, 0x80); // RTF_COLDSTART
        PutByte(addr + ex::RTAG_VERSION, 39);
        PutByte(addr + ex::RTAG_TYPE, type);
        PutByte(addr + ex::RTAG_PRI, static_cast<uint8_t>(pri));
        PutLong(addr + ex::RTAG_NAME, nameAddr);
        PutLong(addr + ex::RTAG_IDSTRING, 0);
        PutLong(addr + ex::RTAG_INIT, addr + 0x40);
        PutCString(nameAddr, name);
    }

    ex::Readers Readers() const
    {
        ex::Readers r;
        r.readLong = [this](uint32_t a, bool& ok) {
            uint32_t v = 0;
            ok = true;
            for (int i = 0; i < 4; ++i) {
                auto it = m_bytes.find(a + static_cast<uint32_t>(i));
                if (it == m_bytes.end()) {
                    ok = false;
                    return 0u;
                }
                v = (v << 8) | it->second;
            }
            return v;
        };
        r.readByte = [this](uint32_t a, bool& ok) -> uint8_t {
            auto it = m_bytes.find(a);
            ok = it != m_bytes.end();
            return ok ? it->second : uint8_t{0};
        };
        r.isMapped = [this](uint32_t a) { return m_bytes.count(a) != 0; };
        return r;
    }

private:
    std::map<uint32_t, uint8_t> m_bytes;
};

constexpr uint32_t kExecBase = 0x00C00276;

std::vector<ResidentScanner::Region> Regions()
{
    return {
        {0x00000000, 0x00080000, "Chip RAM", false},
        {0x00C00000, 0x00080000, "Slow RAM", false},
        {0x00F80000, 0x00080000, "Kickstart ROM", true},
    };
}

} // namespace

TEST(ResidentScannerTest, ReadsAndValidatesAResidentTag)
{
    FakeMem mem;
    mem.PutResident(0x00F81000, "dos.library", 9, 0, 0x00F81900);
    auto m = ResidentScanner::ReadResident(mem.Readers(), 0x00F81000);

    EXPECT_TRUE(m.valid);
    EXPECT_EQ(m.name, "dos.library");
    EXPECT_EQ(m.version, 39);
    EXPECT_EQ(m.type, 9);
    EXPECT_EQ(m.typeName, "Library");
    EXPECT_EQ(m.initFn, 0x00F81040u);
}

TEST(ResidentScannerTest, RejectsATagWhoseMatchTagDoesNotPointAtItself)
{
    FakeMem mem;
    mem.PutResident(0x00F81000, "fake", 9, 0, 0x00F81900);
    mem.PutLong(0x00F81000 + ex::RTAG_MATCHTAG, 0x00F82000); // points elsewhere
    EXPECT_FALSE(ResidentScanner::ReadResident(mem.Readers(), 0x00F81000).valid);

    mem.PutResident(0x00F83000, "fake2", 9, 0, 0x00F83900);
    mem.PutWord(0x00F83000 + ex::RTAG_MATCHWORD, 0x1234); // wrong matchword
    EXPECT_FALSE(ResidentScanner::ReadResident(mem.Readers(), 0x00F83000).valid);
}

TEST(ResidentScannerTest, WalksResModulesToItsNullTerminator)
{
    FakeMem mem;
    const uint32_t array = 0x00F80400;
    mem.PutLong(kExecBase + ex::EB_RESMODULES, array);
    mem.PutResident(0x00F81000, "exec.library", 9, 105, 0x00F81900);
    mem.PutResident(0x00F82000, "trackdisk.device", 3, 0, 0x00F82900);
    mem.PutLong(array + 0, 0x00F81000);
    mem.PutLong(array + 4, 0x00F82000);
    mem.PutLong(array + 8, 0);

    auto mods = ResidentScanner::ListResidentModules(mem.Readers(), kExecBase);
    ASSERT_EQ(mods.size(), 2u);
    EXPECT_EQ(mods[0].name, "exec.library");
    EXPECT_EQ(mods[0].pri, 105);
    EXPECT_EQ(mods[1].typeName, "Device");
    EXPECT_TRUE(mods[1].valid);
    EXPECT_FALSE(mods[1].indirect);
}

TEST(ResidentScannerTest, FollowsTheBit31IndirectionToAnotherArray)
{
    FakeMem mem;
    const uint32_t first = 0x00F80400, second = 0x00020000;
    mem.PutLong(kExecBase + ex::EB_RESMODULES, first);
    mem.PutResident(0x00F81000, "rom.tag", 9, 0, 0x00F81900);
    mem.PutResident(0x00030000, "injected.tag", 9, 0, 0x00030900);

    mem.PutLong(first + 0, 0x00F81000);
    mem.PutLong(first + 4, 0x80000000u | second); // continue over there
    mem.PutLong(second + 0, 0x00030000);
    mem.PutLong(second + 4, 0);

    auto mods = ResidentScanner::ListResidentModules(mem.Readers(), kExecBase);
    ASSERT_EQ(mods.size(), 2u);
    EXPECT_EQ(mods[0].name, "rom.tag");
    EXPECT_FALSE(mods[0].indirect);
    // The second tag lives in RAM, reached only via the redirect — exactly the
    // shape of an injected module.
    EXPECT_EQ(mods[1].name, "injected.tag");
    EXPECT_TRUE(mods[1].indirect);
}

TEST(ResidentScannerTest, EmptyResModulesYieldsNothing)
{
    FakeMem mem;
    mem.PutLong(kExecBase + ex::EB_RESMODULES, 0);
    EXPECT_TRUE(ResidentScanner::ListResidentModules(mem.Readers(), kExecBase).empty());
}

TEST(ResidentScannerTest, RebootSurvivorsReportsCaptureVectors)
{
    FakeMem mem;
    mem.PutLong(kExecBase + ex::EB_COLDCAPTURE, 0);
    mem.PutLong(kExecBase + ex::EB_COOLCAPTURE, 0x00030000); // in RAM: someone hooked it
    mem.PutLong(kExecBase + ex::EB_WARMCAPTURE, 0x00F80100); // in ROM: the stock value
    mem.PutLong(kExecBase + ex::EB_KICKMEMPTR, 0);
    mem.PutLong(kExecBase + ex::EB_KICKTAGPTR, 0);
    mem.PutLong(kExecBase + ex::EB_KICKCHECKSUM, 0);
    mem.PutEmptyList(kExecBase + ex::EB_MEMLIST);

    auto s = ResidentScanner::ScanRebootSurvivors(mem.Readers(), kExecBase, Regions());
    ASSERT_TRUE(s.available);
    ASSERT_EQ(s.captures.size(), 3u);
    EXPECT_FALSE(s.captures[0].set);
    EXPECT_TRUE(s.captures[1].set);
    EXPECT_FALSE(s.captures[1].pointsToRom);
    EXPECT_TRUE(s.captures[2].set);
    EXPECT_TRUE(s.captures[2].pointsToRom);
    EXPECT_TRUE(s.AnySurvivors());
}

TEST(ResidentScannerTest, RebootSurvivorsFollowsKickTagsAndKickMem)
{
    FakeMem mem;
    const uint32_t tagArray = 0x00030000, memList = 0x00031000;
    mem.PutLong(kExecBase + ex::EB_COLDCAPTURE, 0);
    mem.PutLong(kExecBase + ex::EB_COOLCAPTURE, 0);
    mem.PutLong(kExecBase + ex::EB_WARMCAPTURE, 0);
    mem.PutLong(kExecBase + ex::EB_KICKTAGPTR, tagArray);
    mem.PutLong(kExecBase + ex::EB_KICKMEMPTR, memList);
    mem.PutEmptyList(kExecBase + ex::EB_MEMLIST);

    mem.PutResident(0x00032000, "resident.stash", 9, 0, 0x00032900);
    mem.PutLong(tagArray + 0, 0x00032000);
    mem.PutLong(tagArray + 4, 0);

    mem.PutLong(memList + ex::LN_SUCC, 0);
    mem.PutWord(memList + 14, 1); // ml_NumEntries
    mem.PutLong(memList + 16, 0x00032000);
    mem.PutLong(memList + 20, 0x400);

    // Store the checksum this scanner computes so the comparison reports a match.
    uint32_t sum = 0;
    for (uint32_t v : {memList, tagArray}) {
        const uint32_t prev = sum;
        sum += v;
        if (sum < prev)
            ++sum;
    }
    mem.PutLong(kExecBase + ex::EB_KICKCHECKSUM, ~sum);

    auto s = ResidentScanner::ScanRebootSurvivors(mem.Readers(), kExecBase, Regions());
    ASSERT_EQ(s.kickTags.size(), 1u);
    EXPECT_EQ(s.kickTags[0].name, "resident.stash");
    EXPECT_TRUE(s.kickTags[0].valid);
    ASSERT_EQ(s.kickMemEntries.size(), 1u);
    EXPECT_EQ(s.kickMemEntries[0].first, 0x00032000u);
    EXPECT_EQ(s.kickMemEntries[0].second, 0x400u);
    EXPECT_TRUE(s.matchesPointerSum);

    mem.PutLong(kExecBase + ex::EB_KICKCHECKSUM, 0xDEADBEEF);
    EXPECT_FALSE(ResidentScanner::ScanRebootSurvivors(mem.Readers(), kExecBase, Regions())
                     .matchesPointerSum);
}

TEST(ResidentScannerTest, DetectsMemoryStolenOffTheTopOfAPool)
{
    FakeMem mem;
    for (uint32_t off : {ex::EB_COLDCAPTURE, ex::EB_COOLCAPTURE, ex::EB_WARMCAPTURE,
                         ex::EB_KICKMEMPTR, ex::EB_KICKTAGPTR, ex::EB_KICKCHECKSUM})
        mem.PutLong(kExecBase + off, 0);

    const uint32_t mh = 0x00C01000;
    mem.PutList(kExecBase + ex::EB_MEMLIST, {mh});
    mem.PutWord(mh + ex::MH_ATTRIBUTES, 0x0002);
    mem.PutLong(mh + ex::MH_LOWER, 0x00001000);
    mem.PutLong(mh + ex::MH_UPPER, 0x0007F000); // $1000 short of the region end
    mem.PutLong(mh + ex::MH_FREE, 0x1000);
    mem.PutLong(mh + ex::MH_FIRST, 0);
    mem.PutLong(mh + ex::LN_NAME, 0x00C01900);
    mem.PutCString(0x00C01900, "chip memory");

    auto s = ResidentScanner::ScanRebootSurvivors(mem.Readers(), kExecBase, Regions());
    ASSERT_EQ(s.stolenMemory.size(), 1u);
    EXPECT_EQ(s.stolenMemory[0].name, "chip memory");
    EXPECT_EQ(s.stolenMemory[0].upper, 0x0007F000u);
    EXPECT_EQ(s.stolenMemory[0].regionEnd, 0x00080000u);
    EXPECT_EQ(s.stolenMemory[0].stolenBytes, 0x1000u);
    EXPECT_TRUE(s.AnySurvivors());
}

TEST(ResidentScannerTest, APoolThatReachesItsRegionEndIsNotStolenFrom)
{
    FakeMem mem;
    for (uint32_t off : {ex::EB_COLDCAPTURE, ex::EB_COOLCAPTURE, ex::EB_WARMCAPTURE,
                         ex::EB_KICKMEMPTR, ex::EB_KICKTAGPTR, ex::EB_KICKCHECKSUM})
        mem.PutLong(kExecBase + off, 0);

    const uint32_t mh = 0x00C01000;
    mem.PutList(kExecBase + ex::EB_MEMLIST, {mh});
    mem.PutLong(mh + ex::MH_LOWER, 0x00001000);
    mem.PutLong(mh + ex::MH_UPPER, 0x00080000);
    mem.PutLong(mh + ex::MH_FREE, 0x1000);
    mem.PutLong(mh + ex::MH_FIRST, 0);
    mem.PutLong(mh + ex::LN_NAME, 0);

    auto s = ResidentScanner::ScanRebootSurvivors(mem.Readers(), kExecBase, Regions());
    EXPECT_TRUE(s.stolenMemory.empty());
    EXPECT_FALSE(s.AnySurvivors());
}

TEST(ResidentScannerTest, ReportsUnavailableWithoutAnExecBase)
{
    FakeMem mem;
    auto s = ResidentScanner::ScanRebootSurvivors(mem.Readers(), 0, Regions());
    EXPECT_FALSE(s.available);
    EXPECT_NE(s.hint.find("ExecBase"), std::string::npos);
}
