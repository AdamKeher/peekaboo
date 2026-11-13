#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "../src/core/DosBreakpoints.h"

using adm::core::DosBreakpoints;
using adm::core::DosBreakRule;
using adm::core::DosNameMatch;
using adm::core::LvoArgType;

namespace ex = adm::core::exec;

namespace {

// Minimal memory holding one BCPL string, which is how dos.library takes a
// filename.
class FakeMem
{
public:
    void PutBString(uint32_t addr, const std::string& s)
    {
        m_bytes[addr] = static_cast<uint8_t>(s.size());
        for (size_t i = 0; i < s.size(); ++i)
            m_bytes[addr + 1 + static_cast<uint32_t>(i)] = static_cast<uint8_t>(s[i]);
    }
    void PutCString(uint32_t addr, const std::string& s)
    {
        for (size_t i = 0; i < s.size(); ++i)
            m_bytes[addr + static_cast<uint32_t>(i)] = static_cast<uint8_t>(s[i]);
        m_bytes[addr + static_cast<uint32_t>(s.size())] = 0;
    }

    ex::Readers Readers() const
    {
        ex::Readers r;
        r.readByte = [this](uint32_t a, bool& ok) -> uint8_t {
            auto it = m_bytes.find(a);
            ok = it != m_bytes.end();
            return ok ? it->second : uint8_t{0};
        };
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
        return r;
    }

private:
    std::map<uint32_t, uint8_t> m_bytes;
};

constexpr uint32_t kOpenEntry = 0x00C0FFE2;
constexpr uint32_t kNameAddr = 0x00030000;

DosBreakRule OpenRule(const std::string& nameMatch = {})
{
    DosBreakRule r;
    r.library = "dos.library";
    r.function = "Open";
    r.lvo = 30;
    r.entryPc = kOpenEntry;
    r.nameMatch = nameMatch;
    r.argReg = "d1";
    r.argType = LvoArgType::BStr;
    return r;
}

// Reads d1 as the caller's register file would.
DosBreakpoints::ReadRegisterFn RegD1(uint32_t value)
{
    return [value](const std::string& name, uint32_t& out) {
        if (name != "d1")
            return false;
        out = value;
        return true;
    };
}

} // namespace

TEST(DosBreakpointsTest, ParsesMatchModes)
{
    DosNameMatch m{};
    EXPECT_TRUE(DosBreakpoints::ParseMatchMode("", m));
    EXPECT_EQ(m, DosNameMatch::Contains);
    EXPECT_TRUE(DosBreakpoints::ParseMatchMode("EQUALS", m));
    EXPECT_EQ(m, DosNameMatch::Equals);
    EXPECT_TRUE(DosBreakpoints::ParseMatchMode("prefix", m));
    EXPECT_EQ(m, DosNameMatch::Prefix);
    EXPECT_TRUE(DosBreakpoints::ParseMatchMode("endswith", m));
    EXPECT_EQ(m, DosNameMatch::Suffix);
    EXPECT_FALSE(DosBreakpoints::ParseMatchMode("regex", m));
}

TEST(DosBreakpointsTest, MatchesAccordingToTheMode)
{
    using DB = DosBreakpoints;
    EXPECT_TRUE(DB::Matches("df0:c/loader", "loader", DosNameMatch::Contains, true));
    EXPECT_FALSE(DB::Matches("df0:c/dir", "loader", DosNameMatch::Contains, true));

    EXPECT_TRUE(DB::Matches("loader", "loader", DosNameMatch::Equals, true));
    EXPECT_FALSE(DB::Matches("df0:loader", "loader", DosNameMatch::Equals, true));

    EXPECT_TRUE(DB::Matches("df0:c/loader", "df0:", DosNameMatch::Prefix, true));
    EXPECT_TRUE(DB::Matches("df0:c/loader", "loader", DosNameMatch::Suffix, true));

    // AmigaDOS is case-insensitive, so that is the default.
    EXPECT_TRUE(DB::Matches("DF0:C/LOADER", "loader", DosNameMatch::Contains, false));
    EXPECT_FALSE(DB::Matches("DF0:C/LOADER", "loader", DosNameMatch::Contains, true));

    // An empty needle means "everything".
    EXPECT_TRUE(DB::Matches("anything", "", DosNameMatch::Equals, true));
}

TEST(DosBreakpointsTest, AddsAndRemovesRules)
{
    DosBreakpoints bps;
    std::string err;
    const int id = bps.Add(OpenRule(), err);
    ASSERT_NE(id, 0) << err;
    EXPECT_EQ(bps.List().size(), 1u);
    EXPECT_EQ(bps.List()[0].entryPc, kOpenEntry);

    EXPECT_TRUE(bps.Remove(id));
    EXPECT_FALSE(bps.Remove(id));
    EXPECT_TRUE(bps.Empty());
}

TEST(DosBreakpointsTest, RejectsUnresolvedAndDuplicateRules)
{
    DosBreakpoints bps;
    std::string err;

    DosBreakRule unresolved = OpenRule();
    unresolved.entryPc = 0;
    EXPECT_EQ(bps.Add(unresolved, err), 0);
    EXPECT_NE(err.find("entry address"), std::string::npos);

    ASSERT_NE(bps.Add(OpenRule("loader"), err), 0) << err;
    EXPECT_EQ(bps.Add(OpenRule("loader"), err), 0);
    EXPECT_NE(err.find("already set"), std::string::npos);
    // A different predicate on the same entry is a different rule.
    EXPECT_NE(bps.Add(OpenRule("menu"), err), 0) << err;
}

TEST(DosBreakpointsTest, RejectsANamePredicateWithNothingToTest)
{
    DosBreakpoints bps;
    DosBreakRule rule = OpenRule("loader");
    rule.argReg.clear(); // the function has no known string argument
    std::string err;
    EXPECT_EQ(bps.Add(rule, err), 0);
    EXPECT_NE(err.find("nameMatch"), std::string::npos);
}

TEST(DosBreakpointsTest, IgnoresStopsAtOtherAddresses)
{
    DosBreakpoints bps;
    std::string err;
    ASSERT_NE(bps.Add(OpenRule(), err), 0) << err;

    FakeMem mem;
    auto d = bps.OnBreak(0x00001234, RegD1(0), mem.Readers());
    EXPECT_FALSE(d.ours);
    EXPECT_FALSE(d.resume);
}

TEST(DosBreakpointsTest, WithoutAPredicateEveryCallStops)
{
    DosBreakpoints bps;
    std::string err;
    ASSERT_NE(bps.Add(OpenRule(), err), 0) << err;

    FakeMem mem;
    auto d = bps.OnBreak(kOpenEntry, RegD1(0), mem.Readers());
    EXPECT_TRUE(d.ours);
    EXPECT_FALSE(d.resume);
    EXPECT_EQ(bps.List()[0].hits, 1u);
    EXPECT_EQ(bps.List()[0].breaks, 1u);
}

TEST(DosBreakpointsTest, ResumesSilentlyWhenTheNameDoesNotMatch)
{
    DosBreakpoints bps;
    std::string err;
    ASSERT_NE(bps.Add(OpenRule("loader"), err), 0) << err;

    FakeMem mem;
    mem.PutBString(kNameAddr, "df0:s/startup-sequence");
    auto d = bps.OnBreak(kOpenEntry, RegD1(kNameAddr >> 2), mem.Readers());
    EXPECT_TRUE(d.ours);
    EXPECT_TRUE(d.resume);
    EXPECT_EQ(d.value, "df0:s/startup-sequence");
    EXPECT_EQ(bps.List()[0].hits, 1u);
    EXPECT_EQ(bps.List()[0].breaks, 0u); // seen, but not stopped on
    EXPECT_EQ(bps.List()[0].lastValue, "df0:s/startup-sequence");
}

TEST(DosBreakpointsTest, StopsWhenTheNameMatches)
{
    DosBreakpoints bps;
    std::string err;
    ASSERT_NE(bps.Add(OpenRule("loader"), err), 0) << err;

    FakeMem mem;
    mem.PutBString(kNameAddr, "df0:c/Loader");
    auto d = bps.OnBreak(kOpenEntry, RegD1(kNameAddr >> 2), mem.Readers());
    EXPECT_TRUE(d.ours);
    EXPECT_FALSE(d.resume); // case-insensitive by default
    EXPECT_EQ(d.value, "df0:c/Loader");
    EXPECT_EQ(bps.List()[0].breaks, 1u);
}

TEST(DosBreakpointsTest, ReadsCStringArgumentsToo)
{
    DosBreakpoints bps;
    DosBreakRule rule = OpenRule("dos.library");
    rule.function = "OpenLibrary";
    rule.argReg = "a1";
    rule.argType = LvoArgType::CStr;
    std::string err;
    ASSERT_NE(bps.Add(rule, err), 0) << err;

    FakeMem mem;
    mem.PutCString(kNameAddr, "dos.library");
    auto readA1 = [](const std::string& name, uint32_t& out) {
        if (name != "a1")
            return false;
        out = kNameAddr;
        return true;
    };
    auto d = bps.OnBreak(kOpenEntry, readA1, mem.Readers());
    EXPECT_TRUE(d.ours);
    EXPECT_FALSE(d.resume);
    EXPECT_EQ(d.value, "dos.library");
}

TEST(DosBreakpointsTest, StopsRatherThanGuessingWhenTheRegisterIsUnreadable)
{
    DosBreakpoints bps;
    std::string err;
    ASSERT_NE(bps.Add(OpenRule("loader"), err), 0) << err;

    FakeMem mem;
    // Silently resuming here would hide the call entirely, so a stop is the
    // safe answer.
    auto d = bps.OnBreak(kOpenEntry, nullptr, mem.Readers());
    EXPECT_TRUE(d.ours);
    EXPECT_FALSE(d.resume);
    EXPECT_EQ(bps.List()[0].breaks, 1u);
}

TEST(DosBreakpointsTest, OneShotRulesRemoveThemselvesOnceTheyStop)
{
    DosBreakpoints bps;
    DosBreakRule rule = OpenRule("loader");
    rule.oneShot = true;
    std::string err;
    ASSERT_NE(bps.Add(rule, err), 0) << err;

    FakeMem mem;
    mem.PutBString(kNameAddr, "menu");
    // A non-matching call must not consume the one shot.
    auto miss = bps.OnBreak(kOpenEntry, RegD1(kNameAddr >> 2), mem.Readers());
    EXPECT_TRUE(miss.resume);
    EXPECT_EQ(bps.List().size(), 1u);

    mem.PutBString(kNameAddr, "loader");
    auto hit = bps.OnBreak(kOpenEntry, RegD1(kNameAddr >> 2), mem.Readers());
    EXPECT_FALSE(hit.resume);
    EXPECT_TRUE(bps.Empty());
}

TEST(DosBreakpointsTest, ClearRemovesEverything)
{
    DosBreakpoints bps;
    std::string err;
    ASSERT_NE(bps.Add(OpenRule("a"), err), 0) << err;
    ASSERT_NE(bps.Add(OpenRule("b"), err), 0) << err;
    bps.Clear();
    EXPECT_TRUE(bps.Empty());

    FakeMem mem;
    EXPECT_FALSE(bps.OnBreak(kOpenEntry, RegD1(0), mem.Readers()).ours);
}
