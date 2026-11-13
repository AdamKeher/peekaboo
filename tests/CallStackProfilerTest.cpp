#include <gtest/gtest.h>

#include "core/CallStackProfiler.h"

using adm::core::CallFrame;
using adm::core::CallStackProfiler;
using adm::core::TraceCf;
using adm::core::TraceStep;

namespace {

// Fake functions by PC range, for a deterministic resolver.
struct FakeFn
{
    uint32_t lo, hi, addr;
    const char* name;
};
constexpr FakeFn kA{0x100, 0x200, 0x100, "A"};
constexpr FakeFn kB{0x200, 0x300, 0x200, "B"};
constexpr FakeFn kC{0x300, 0x400, 0x300, "C"};
constexpr FakeFn kH{0x1000, 0x1100, 0x1000, "H"}; // interrupt handler

CallStackProfiler::ResolveFn MakeResolver()
{
    return [](uint32_t pc) -> CallStackProfiler::ResolvedFn {
        for (const FakeFn& f : {kA, kB, kC, kH}) {
            if (pc >= f.lo && pc < f.hi) {
                return {f.addr, f.name, true};
            }
        }
        return {0, "", false};
    };
}

// A stable stack pointer for non-interrupt steps (no exception-frame drop).
constexpr uint32_t kBaseSp = 0x8000;

// Build a step. lRef is assigned by the caller's running index. `sp` defaults to a
// fixed value so ordinary steps show no SP change (only interrupts drop the SP).
TraceStep Step(uint32_t pc, TraceCf cf, uint32_t idx, uint32_t sp = kBaseSp, uint8_t len = 2)
{
    TraceStep s;
    s.pc = pc;
    s.startLRef = idx * 4;
    s.endLRef = (idx + 1) * 4;
    s.sp = sp;
    s.len = len;
    s.cf = cf;
    return s;
}

// Find the frame at a given depth whose function name matches (first match).
const CallFrame* FindFrame(const std::vector<CallFrame>& frames, int depth, const std::string& name)
{
    for (const auto& f : frames) {
        if (f.depth == depth && f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace

TEST(CallStackProfilerTest, SimpleCallReturn)
{
    std::vector<TraceStep> steps = {
        Step(0x100, TraceCf::Seq, 0),    // A
        Step(0x102, TraceCf::Call, 1),   // A: BSR B
        Step(0x200, TraceCf::Seq, 2),    // B entry (depth 1)
        Step(0x202, TraceCf::Return, 3), // B: RTS
        Step(0x104, TraceCf::Seq, 4),    // back in A
    };
    CallStackProfiler p;
    p.Update(steps, MakeResolver());

    EXPECT_EQ(p.MaxDepth(), 1);
    const CallFrame* a = FindFrame(p.Frames(), 0, "A");
    const CallFrame* b = FindFrame(p.Frames(), 1, "B");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->startLRef, 0u);
    EXPECT_EQ(b->startLRef, 2u * 4u); // B entered at step index 2
    EXPECT_EQ(b->endLRef, 4u * 4u);   // popped when execution returned to A (step 4)
    EXPECT_EQ(a->endLRef, 5u * 4u);   // A closes at end of trace
}

TEST(CallStackProfilerTest, NestedCalls)
{
    std::vector<TraceStep> steps = {
        Step(0x100, TraceCf::Call, 0),   // A calls B
        Step(0x200, TraceCf::Seq, 1),    // B
        Step(0x202, TraceCf::Call, 2),   // B calls C
        Step(0x300, TraceCf::Seq, 3),    // C
        Step(0x302, TraceCf::Return, 4), // C returns
        Step(0x204, TraceCf::Return, 5), // B returns
        Step(0x102, TraceCf::Seq, 6),    // back in A
    };
    CallStackProfiler p;
    p.Update(steps, MakeResolver());

    EXPECT_EQ(p.MaxDepth(), 2);
    EXPECT_NE(FindFrame(p.Frames(), 0, "A"), nullptr);
    EXPECT_NE(FindFrame(p.Frames(), 1, "B"), nullptr);
    EXPECT_NE(FindFrame(p.Frames(), 2, "C"), nullptr);
}

TEST(CallStackProfilerTest, RecursionStacksSameFunction)
{
    std::vector<TraceStep> steps = {
        Step(0x100, TraceCf::Call, 0),   // A calls A
        Step(0x100, TraceCf::Seq, 1),    // A (recursive, depth 1)
        Step(0x102, TraceCf::Return, 2), // inner A returns
        Step(0x102, TraceCf::Seq, 3),    // outer A
    };
    CallStackProfiler p;
    p.Update(steps, MakeResolver());

    EXPECT_EQ(p.MaxDepth(), 1);
    // Two distinct frames for A, at depth 0 and 1.
    int aCount = 0;
    for (const auto& f : p.Frames()) {
        if (f.name == "A")
            ++aCount;
    }
    EXPECT_EQ(aCount, 2);
    EXPECT_NE(FindFrame(p.Frames(), 0, "A"), nullptr);
    EXPECT_NE(FindFrame(p.Frames(), 1, "A"), nullptr);
}

TEST(CallStackProfilerTest, InterruptEntryPushesAndRtePops)
{
    // The interrupt entry drops SP by a 68000 exception frame (6 bytes); the RTE
    // restores it. This SP drop is what distinguishes a real interrupt from a
    // spurious PC divergence.
    std::vector<TraceStep> steps = {
        Step(0x100, TraceCf::Seq, 0, kBaseSp),         // A
        Step(0x102, TraceCf::Seq, 1, kBaseSp),         // A (next would be 0x104)
        Step(0x1000, TraceCf::Seq, 2, kBaseSp - 6),    // interrupt! SP dropped -> push H
        Step(0x1002, TraceCf::Return, 3, kBaseSp - 6), // H: RTE
        Step(0x104, TraceCf::Seq, 4, kBaseSp),         // back in A, SP restored
    };
    CallStackProfiler p;
    p.Update(steps, MakeResolver());

    EXPECT_EQ(p.MaxDepth(), 1);
    const CallFrame* h = FindFrame(p.Frames(), 1, "H");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->startLRef, 2u * 4u);
    EXPECT_EQ(h->endLRef, 4u * 4u); // popped when execution returned to A
    // A stays at depth 0 the whole time.
    EXPECT_NE(FindFrame(p.Frames(), 0, "A"), nullptr);
}

TEST(CallStackProfilerTest, SpuriousDivergenceWithoutSpDropStaysFlat)
{
    // A PC divergence with the SP unchanged is NOT an interrupt (it's a mis-sized
    // instruction or a trace gap). It must not push a frame, or depth runs away —
    // this is the voxel_interrupt_handler-stacked-8-deep regression.
    std::vector<TraceStep> steps = {
        Step(0x100, TraceCf::Seq, 0, kBaseSp), Step(0x102, TraceCf::Seq, 1, kBaseSp),
        Step(0x140, TraceCf::Seq, 2, kBaseSp), // pc jumped within A, SP unchanged
        Step(0x142, TraceCf::Seq, 3, kBaseSp), Step(0x144, TraceCf::Seq, 4, kBaseSp),
    };
    CallStackProfiler p;
    p.Update(steps, MakeResolver());
    EXPECT_EQ(p.MaxDepth(), 0); // stays flat — no spurious nesting
}

TEST(CallStackProfilerTest, TailJumpChangesFunctionSameDepth)
{
    std::vector<TraceStep> steps = {
        Step(0x100, TraceCf::Jump, 0), // A: JMP B (tail dispatch)
        Step(0x200, TraceCf::Seq, 1),  // B, same depth
    };
    CallStackProfiler p;
    p.Update(steps, MakeResolver());

    EXPECT_EQ(p.MaxDepth(), 0);
    EXPECT_NE(FindFrame(p.Frames(), 0, "A"), nullptr);
    EXPECT_NE(FindFrame(p.Frames(), 0, "B"), nullptr);
}

TEST(CallStackProfilerTest, ReturnUnderflowReseedsRoot)
{
    // Trace begins mid-call: the first return would pop the seed with nothing left.
    std::vector<TraceStep> steps = {
        Step(0x100, TraceCf::Return, 0), // A returns immediately
        Step(0x200, TraceCf::Seq, 1),    // B — caller we never saw the call for
        Step(0x202, TraceCf::Seq, 2),    // B
    };
    CallStackProfiler p;
    p.Update(steps, MakeResolver());

    // Should not crash; B becomes a root frame at depth 0.
    EXPECT_GE(p.MaxDepth(), 0);
    EXPECT_NE(FindFrame(p.Frames(), 0, "B"), nullptr);
}

TEST(CallStackProfilerTest, UnresolvedFunctionsAreKept)
{
    std::vector<TraceStep> steps = {
        Step(0x100, TraceCf::Call, 0),   // A calls into ROM
        Step(0xF80000, TraceCf::Seq, 1), // unresolved (no fake fn covers it)
    };
    CallStackProfiler p;
    p.Update(steps, MakeResolver());

    bool haveUnresolved = false;
    for (const auto& f : p.Frames()) {
        if (f.funcAddress == 0 && f.name.empty())
            haveUnresolved = true;
    }
    EXPECT_TRUE(haveUnresolved);
}

TEST(CallStackProfilerTest, EmptyTraceProducesNoFrames)
{
    CallStackProfiler p;
    p.Update({}, MakeResolver());
    EXPECT_TRUE(p.Frames().empty());
    EXPECT_EQ(p.MaxDepth(), 0);
}

TEST(CallStackProfilerTest, ClearBumpsVersion)
{
    CallStackProfiler p;
    p.Update({Step(0x100, TraceCf::Seq, 0)}, MakeResolver());
    uint32_t v = p.GetVersion();
    p.Clear();
    EXPECT_TRUE(p.Frames().empty());
    EXPECT_GT(p.GetVersion(), v);
}
