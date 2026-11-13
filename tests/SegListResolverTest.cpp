#include <gtest/gtest.h>

#include <unordered_map>

#include "core/SegListResolver.h"

using adm::core::AmigaSegment;
using adm::core::SegListResolver;

namespace {

// A sparse big-endian memory image for exercising the seglist walk. Any address
// not explicitly written reads back as "unmapped" (ok=false), which lets the
// tests confirm the resolver bails instead of reading garbage.
class FakeMem
{
public:
    void SetByte(uint32_t addr, uint8_t v) { m_bytes[addr] = v; }
    void SetLong(uint32_t addr, uint32_t v)
    {
        m_bytes[addr + 0] = static_cast<uint8_t>(v >> 24);
        m_bytes[addr + 1] = static_cast<uint8_t>(v >> 16);
        m_bytes[addr + 2] = static_cast<uint8_t>(v >> 8);
        m_bytes[addr + 3] = static_cast<uint8_t>(v);
    }

    SegListResolver::ReadLongFn LongFn() const
    {
        return [this](uint32_t addr, bool& ok) -> uint32_t {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
                auto it = m_bytes.find(addr + i);
                if (it == m_bytes.end()) {
                    ok = false;
                    return 0;
                }
                v = (v << 8) | it->second;
            }
            return v;
        };
    }

    SegListResolver::ReadByteFn ByteFn() const
    {
        return [this](uint32_t addr, bool& ok) -> uint8_t {
            auto it = m_bytes.find(addr);
            if (it == m_bytes.end()) {
                ok = false;
                return 0;
            }
            return it->second;
        };
    }

private:
    std::unordered_map<uint32_t, uint8_t> m_bytes;
};

// AmigaOS offsets mirrored from the implementation.
constexpr uint32_t kThisTask = 276;
constexpr uint32_t kNodeType = 8;
constexpr uint32_t kPrCli = 172;
constexpr uint32_t kCliModule = 60;
constexpr uint32_t kPrSegList = 128;
constexpr uint32_t kSegArray3 = 12;
constexpr uint8_t kNtProcess = 13;

constexpr uint32_t kExecBase = 0x1000;
constexpr uint32_t kTask = 0x2000;
constexpr uint32_t kCli = 0x3000;
constexpr uint32_t kSeg1 = 0x4000; // 0x100-byte alloc -> base 0x4004, size 0xF8
constexpr uint32_t kSeg2 = 0x5000; // 0x80-byte alloc  -> base 0x5004, size 0x78

inline uint32_t ToBptr(uint32_t aptr) { return aptr >> 2; }

// Build a valid CLI-launched process with a two-segment list.
FakeMem BuildCliProcess()
{
    FakeMem m;
    m.SetLong(4, kExecBase); // ExecBase pointer
    m.SetLong(kExecBase + kThisTask, kTask);
    m.SetByte(kTask + kNodeType, kNtProcess);
    m.SetLong(kTask + kPrCli, ToBptr(kCli));
    m.SetLong(kCli + kCliModule, ToBptr(kSeg1));

    m.SetLong(kSeg1 - 4, 0x100);     // segment 1 allocation length in BYTES
    m.SetLong(kSeg1, ToBptr(kSeg2)); // next BPTR
    m.SetLong(kSeg2 - 4, 0x80);      // segment 2 allocation length in BYTES
    m.SetLong(kSeg2, 0);             // end of chain
    return m;
}

} // namespace

TEST(SegListResolverTest, WalksCliSegmentList)
{
    FakeMem m = BuildCliProcess();
    auto segs = SegListResolver::ResolveActiveProcess(m.LongFn(), m.ByteFn());
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].base, kSeg1 + 4);
    EXPECT_EQ(segs[0].size, 0x100u - 8u);
    EXPECT_EQ(segs[1].base, kSeg2 + 4);
    EXPECT_EQ(segs[1].size, 0x80u - 8u);
}

// Regression: the allocation longword before a segment node is a BYTE count (the AllocMem
// size UnLoadSeg passes to FreeMem), not a longword count. Reading it as longwords inflated
// every segment 4x, which made FindSegmentContaining claim addresses outside the program and
// gave FunctionProfiler an over-wide attribution window.
// Values below are the real measured case: a bartman .text of 0xC2C8 bytes.
TEST(SegListResolverTest, AllocationLengthIsBytesNotLongwords)
{
    FakeMem m = BuildCliProcess();
    constexpr uint32_t kTextAlloc = 0xC2D4; // observed allocation for a 0xC2C8-byte .text
    m.SetLong(kSeg1 - 4, kTextAlloc);

    auto segs = SegListResolver::ResolveActiveProcess(m.LongFn(), m.ByteFn());
    ASSERT_FALSE(segs.empty());
    EXPECT_EQ(segs[0].size, kTextAlloc - 8u); // 0xC2CC, NOT 0xC2D4 * 4 - 8
    EXPECT_LT(segs[0].size, 0x10000u);        // would have been ~0x30B48 under the old maths

    // The segment must not swallow addresses past its own allocation.
    EXPECT_TRUE(
        SegListResolver::FindSegmentContaining(kSeg1 + 4, m.LongFn(), m.ByteFn()).has_value());
    EXPECT_FALSE(
        SegListResolver::FindSegmentContaining(kSeg1 + 4 + kTextAlloc, m.LongFn(), m.ByteFn())
            .has_value());
}

TEST(SegListResolverTest, ProgramBaseIsFirstSegment)
{
    FakeMem m = BuildCliProcess();
    auto base = SegListResolver::ProgramBase(m.LongFn(), m.ByteFn());
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(*base, kSeg1 + 4);
}

TEST(SegListResolverTest, FindSegmentContainingPc)
{
    FakeMem m = BuildCliProcess();
    auto s = SegListResolver::FindSegmentContaining(kSeg1 + 0x50, m.LongFn(), m.ByteFn());
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->base, kSeg1 + 4);

    // Address inside segment 2.
    auto s2 = SegListResolver::FindSegmentContaining(kSeg2 + 0x10, m.LongFn(), m.ByteFn());
    ASSERT_TRUE(s2.has_value());
    EXPECT_EQ(s2->base, kSeg2 + 4);
}

TEST(SegListResolverTest, PcOutsideAllSegmentsReturnsNone)
{
    FakeMem m = BuildCliProcess();
    auto s = SegListResolver::FindSegmentContaining(0x99999, m.LongFn(), m.ByteFn());
    EXPECT_FALSE(s.has_value());
}

TEST(SegListResolverTest, FallsBackToPrSegListWhenNoCli)
{
    FakeMem m;
    constexpr uint32_t kSegArray = 0x6000;
    m.SetLong(4, kExecBase);
    m.SetLong(kExecBase + kThisTask, kTask);
    m.SetByte(kTask + kNodeType, kNtProcess);
    m.SetLong(kTask + kPrCli, 0); // no CLI
    m.SetLong(kTask + kPrSegList, ToBptr(kSegArray));
    m.SetLong(kSegArray + kSegArray3, ToBptr(kSeg1));
    m.SetLong(kSeg1 - 4, 0x40);
    m.SetLong(kSeg1, 0); // single segment

    auto base = SegListResolver::ProgramBase(m.LongFn(), m.ByteFn());
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(*base, kSeg1 + 4);
}

TEST(SegListResolverTest, NonProcessTaskReturnsEmpty)
{
    FakeMem m = BuildCliProcess();
    // Overwrite the node type so it is a plain task, not a process.
    m.SetByte(kTask + kNodeType, 1 /* NT_TASK */);
    auto segs = SegListResolver::ResolveActiveProcess(m.LongFn(), m.ByteFn());
    EXPECT_TRUE(segs.empty());
}

TEST(SegListResolverTest, NullExecBaseReturnsEmpty)
{
    FakeMem m; // nothing mapped: reading ExecBase fails
    auto segs = SegListResolver::ResolveActiveProcess(m.LongFn(), m.ByteFn());
    EXPECT_TRUE(segs.empty());
    EXPECT_FALSE(SegListResolver::ProgramBase(m.LongFn(), m.ByteFn()).has_value());
}

TEST(SegListResolverTest, StopsOnUndersizedAllocation)
{
    FakeMem m = BuildCliProcess();
    m.SetLong(kSeg1 - 4, 4); // fewer than the two bookkeeping longwords (8 bytes)
    auto segs = SegListResolver::ResolveActiveProcess(m.LongFn(), m.ByteFn());
    EXPECT_TRUE(segs.empty());
}
