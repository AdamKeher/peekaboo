#include <gtest/gtest.h>

#include "core/DebugFrame.h"
#include "core/DebugInline.h"
#include "core/DebugSymbols.h"
#include "core/StackUnwinder.h"

#include <filesystem>
#include <map>
#include <string>

using adm::core::CfiRow;
using adm::core::DebugFrame;
using adm::core::DebugInline;
using adm::core::DebugSymbols;
using adm::core::StackUnwinder;
using adm::core::UnwindInputs;
using adm::core::UnwindResult;
using adm::core::UnwindStop;

namespace {

std::string FixturePath()
{
    for (const char* candidate : {"tests/images/a.elf", "../tests/images/a.elf",
                                  "../../tests/images/a.elf", "../../../tests/images/a.elf"}) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

// Link-time addresses in the fixture, from its symbol table.
constexpr uint32_t kMain = 0x84;           // 9234 bytes
constexpr uint32_t kDisableCache = 0x2498; // 84 bytes
constexpr uint32_t kVoxelC2pKick = 0x264c; // 996 bytes
constexpr uint32_t kInlineSite = 0x268e;   // inside voxel_c2p_kick, two functions inlined here

// Where the pretend program is loaded. Deliberately not zero, so a missing
// runtime->link translation shows up as a wrong answer rather than passing by luck.
constexpr uint32_t kLoadBase = 0x00021000;
constexpr uint32_t kProgramSize = 0x8000;

// A stack that reads as real memory: every longword in range is readable and zero
// unless explicitly written. Reads outside the range fail, the way an unmapped
// address does on the real machine.
class FakeStack
{
public:
    FakeStack(uint32_t lo, uint32_t hi) : m_lo(lo), m_hi(hi) {}

    void PutLong(uint32_t addr, uint32_t value) { m_written[addr] = value; }

    bool Read(uint32_t addr, uint32_t& out) const
    {
        if (addr < m_lo || addr + 4 > m_hi) {
            return false;
        }
        auto it = m_written.find(addr);
        out = (it == m_written.end()) ? 0u : it->second;
        return true;
    }

private:
    uint32_t m_lo, m_hi;
    std::map<uint32_t, uint32_t> m_written;
};

class StackUnwinderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const std::string path = FixturePath();
        if (path.empty()) {
            GTEST_SKIP() << "tests/images/a.elf fixture not found";
        }
        ASSERT_TRUE(m_frame.LoadFromElf(path));
        ASSERT_TRUE(m_symbols.LoadFromElf(path));
        m_inline.LoadFromElf(path); // optional: inline expansion is asserted separately
    }

    // The CFA offset the real CFI gives for a link address — used to place return
    // addresses at the slots the unwinder will actually look in, so this test exercises
    // the unwinder rather than restating the CFI table (DebugFrameTest covers that).
    int32_t CfaOffsetAt(uint32_t linkAddr) const
    {
        CfiRow row;
        EXPECT_TRUE(m_frame.ResolveRow(linkAddr, row)) << std::hex << linkAddr;
        return row.cfaOffset;
    }

    UnwindInputs MakeInputs(const FakeStack& stack, uint32_t pc, uint32_t sp) const
    {
        UnwindInputs in;
        in.regs.pc = pc;
        in.regs.reg[adm::core::kDwarfRegSp] = sp;
        in.readLong = [&stack](uint32_t addr, uint32_t& out) { return stack.Read(addr, out); };
        in.toLink = [](uint32_t addr) -> std::optional<uint32_t> {
            if (addr < kLoadBase || addr >= kLoadBase + kProgramSize) {
                return std::nullopt;
            }
            return addr - kLoadBase;
        };
        in.frames = &m_frame;
        in.symbols = &m_symbols;
        return in;
    }

    DebugFrame m_frame;
    DebugSymbols m_symbols;
    DebugInline m_inline;
};

} // namespace

TEST_F(StackUnwinderTest, WalksTwoRealFramesAndNamesThem)
{
    FakeStack stack(0x70000, 0x81000);

    // Frame 0: inside system_disable_cache, past the a6 push so the CFA is a7 + 20.
    const uint32_t sp0 = 0x80000;
    const int32_t cfaOff0 = CfaOffsetAt(kDisableCache + 10);
    ASSERT_EQ(cfaOff0, 20);
    const uint32_t cfa0 = sp0 + static_cast<uint32_t>(cfaOff0);

    // Its caller is main, 200 bytes in. The return address lives at CFA - 4.
    const uint32_t retToMain = kLoadBase + kMain + 200;
    stack.PutLong(cfa0 - 4, retToMain);

    // Frame 1: main's own CFA, anchored on the stack pointer the first unwind produced.
    const uint32_t sp1 = cfa0;
    const uint32_t cfa1 = sp1 + static_cast<uint32_t>(CfaOffsetAt(kMain + 198));
    stack.PutLong(cfa1 - 4, 0); // a zero return address ends the chain, as _start leaves it

    UnwindInputs in = MakeInputs(stack, kLoadBase + kDisableCache + 10, sp0);
    const UnwindResult r = StackUnwinder::Unwind(in);

    ASSERT_EQ(r.frames.size(), 2u);
    EXPECT_EQ(r.frames[0].name, "system_disable_cache");
    EXPECT_EQ(r.frames[0].pc, kLoadBase + kDisableCache + 10);
    EXPECT_EQ(r.frames[0].sp, sp0);
    EXPECT_EQ(r.frames[0].cfa, cfa0);

    EXPECT_EQ(r.frames[1].name, "main");
    EXPECT_EQ(r.frames[1].pc, retToMain);
    EXPECT_EQ(r.frames[1].sp, sp1);
    EXPECT_EQ(r.frames[1].cfa, cfa1);

    EXPECT_EQ(r.stop, UnwindStop::EndOfStack);
}

TEST_F(StackUnwinderTest, RestoresCalleeSavedRegistersFromTheirCfaSlots)
{
    // main saves a6 at CFA-168. The unwinder must recover the caller's copy, because in
    // general a later frame's CFA can be expressed against a restored register.
    FakeStack stack(0x70000, 0x81000);
    constexpr uint32_t kA6 = 14;
    constexpr uint32_t kSavedA6 = 0xDEADBEE0;

    const uint32_t sp0 = 0x80000;
    const uint32_t cfa0 = sp0 + static_cast<uint32_t>(CfaOffsetAt(kMain + 4000));

    CfiRow row;
    ASSERT_TRUE(m_frame.ResolveRow(kMain + 4000, row));
    ASSERT_EQ(row.reg[kA6].kind, adm::core::CfiRegRuleKind::OffsetCfa);
    stack.PutLong(cfa0 + static_cast<uint32_t>(row.reg[kA6].offset), kSavedA6);
    stack.PutLong(cfa0 - 4, 0); // end the chain right after this frame

    UnwindInputs in = MakeInputs(stack, kLoadBase + kMain + 4000, sp0);
    in.regs.reg[kA6] = 0x11111111; // the value in the *current* frame, which must not leak
    const UnwindResult r = StackUnwinder::Unwind(in);

    ASSERT_EQ(r.frames.size(), 1u);
    EXPECT_EQ(r.frames[0].name, "main");
    EXPECT_EQ(r.stop, UnwindStop::EndOfStack);
}

TEST_F(StackUnwinderTest, ExpandsInlinedFramesInnermostFirst)
{
    if (!m_inline.IsLoaded()) {
        GTEST_SKIP() << "fixture has no usable .debug_info inline data";
    }
    const std::vector<std::string> expected = m_inline.ResolveInlineStack(kInlineSite);
    if (expected.size() < 2) {
        GTEST_SKIP() << "chosen address is not an inlined site in this fixture";
    }

    FakeStack stack(0x70000, 0x81000);
    const uint32_t sp0 = 0x80000;
    const uint32_t cfa0 = sp0 + static_cast<uint32_t>(CfaOffsetAt(kInlineSite));
    stack.PutLong(cfa0 - 4, 0);

    UnwindInputs in = MakeInputs(stack, kLoadBase + kInlineSite, sp0);
    in.inlines = &m_inline;
    const UnwindResult r = StackUnwinder::Unwind(in);

    // DebugInline reports outermost-first; a backtrace reads innermost-first, so the
    // order must be reversed and everything except the real function tagged inlined.
    ASSERT_EQ(r.frames.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(r.frames[i].name, expected[expected.size() - 1 - i]) << "at depth " << i;
    }
    for (size_t i = 0; i + 1 < r.frames.size(); ++i) {
        EXPECT_TRUE(r.frames[i].inlined) << "at depth " << i;
    }
    EXPECT_FALSE(r.frames.back().inlined);

    // Inlined frames share the real frame's addresses -- they have no frame of their own.
    for (const auto& f : r.frames) {
        EXPECT_EQ(f.cfa, cfa0);
        EXPECT_EQ(f.sp, sp0);
    }
}

TEST_F(StackUnwinderTest, StopsWithNoCfiWhenThePcLeavesTheProgram)
{
    FakeStack stack(0x70000, 0x81000);
    const uint32_t sp0 = 0x80000;
    const uint32_t cfa0 = sp0 + static_cast<uint32_t>(CfaOffsetAt(kDisableCache + 10));

    // A Kickstart return address: real code, but nothing this ELF describes.
    constexpr uint32_t kRomReturn = 0x00FC1A2E;
    stack.PutLong(cfa0 - 4, kRomReturn);

    UnwindInputs in = MakeInputs(stack, kLoadBase + kDisableCache + 10, sp0);
    const UnwindResult r = StackUnwinder::Unwind(in);

    ASSERT_EQ(r.frames.size(), 1u);
    EXPECT_EQ(r.frames[0].name, "system_disable_cache");
    EXPECT_EQ(r.stop, UnwindStop::NoCfi);
    EXPECT_EQ(r.stopPc, kRomReturn);
    EXPECT_EQ(adm::core::DescribeUnwindStop(r, "Kickstart ROM"),
              "no CFI beyond $00FC1A2E (Kickstart ROM)");
}

TEST_F(StackUnwinderTest, StopsWhenTheStackReadFallsOutsideMemory)
{
    // A stack region that ends before the slot holding the return address.
    FakeStack stack(0x70000, 0x80000);
    UnwindInputs in = MakeInputs(stack, kLoadBase + kDisableCache + 10, 0x7FFF0);
    const UnwindResult r = StackUnwinder::Unwind(in);

    EXPECT_EQ(r.stop, UnwindStop::UnmappedMemory);
    EXPECT_EQ(r.frames.size(), 1u);
}

TEST_F(StackUnwinderTest, RejectsAnOddReturnAddress)
{
    FakeStack stack(0x70000, 0x81000);
    const uint32_t sp0 = 0x80000;
    const uint32_t cfa0 = sp0 + static_cast<uint32_t>(CfaOffsetAt(kDisableCache + 10));
    stack.PutLong(cfa0 - 4, kLoadBase + kMain + 201); // odd: cannot be an m68k instruction

    UnwindInputs in = MakeInputs(stack, kLoadBase + kDisableCache + 10, sp0);
    const UnwindResult r = StackUnwinder::Unwind(in);

    ASSERT_EQ(r.frames.size(), 1u);
    EXPECT_EQ(r.stop, UnwindStop::UnmappedMemory);
}

TEST_F(StackUnwinderTest, HonoursTheDepthLimit)
{
    // A stack of frames that all return into the same function. Each level consumes
    // 20 bytes, so a generous stack region keeps it going until the cap bites.
    FakeStack stack(0x70000, 0x90000);
    const uint32_t pc = kLoadBase + kDisableCache + 10;
    const uint32_t cfaOff = static_cast<uint32_t>(CfaOffsetAt(kDisableCache + 10));

    uint32_t sp = 0x80000;
    for (int i = 0; i < 40; ++i) {
        const uint32_t cfa = sp + cfaOff;
        stack.PutLong(cfa - 4, pc);
        sp = cfa;
    }

    UnwindInputs in = MakeInputs(stack, pc, 0x80000);
    in.maxDepth = 8;
    const UnwindResult r = StackUnwinder::Unwind(in);

    EXPECT_EQ(r.frames.size(), 8u);
    EXPECT_EQ(r.stop, UnwindStop::DepthLimit);
}

TEST_F(StackUnwinderTest, ReportsMissingDebugInfoRatherThanGuessing)
{
    FakeStack stack(0x70000, 0x81000);
    UnwindInputs in = MakeInputs(stack, kLoadBase + kDisableCache, 0x80000);
    in.frames = nullptr;

    const UnwindResult r = StackUnwinder::Unwind(in);
    EXPECT_TRUE(r.Empty());
    EXPECT_EQ(r.stop, UnwindStop::NoDebugInfo);
    EXPECT_EQ(adm::core::DescribeUnwindStop(r, ""),
              "no debug info: load a program with a sibling .elf");
}

TEST_F(StackUnwinderTest, AdjustsTheLookupAddressForCallerFrames)
{
    // A return address that lands exactly on a function boundary belongs, unadjusted, to
    // the *next* function. Unwinding must attribute it to the call site instead --
    // the same adjustment gdb makes.
    FakeStack stack(0x70000, 0x81000);
    const uint32_t sp0 = 0x80000;
    const uint32_t cfa0 = sp0 + static_cast<uint32_t>(CfaOffsetAt(kDisableCache + 10));

    // voxel_c2p_kick ends at 0x264c + 996 == 0x2a30, which is where the next FDE starts.
    const uint32_t retAtBoundary = kLoadBase + kVoxelC2pKick + 996;
    stack.PutLong(cfa0 - 4, retAtBoundary);

    const uint32_t sp1 = cfa0;
    const uint32_t cfa1 = sp1 + static_cast<uint32_t>(CfaOffsetAt(kVoxelC2pKick + 994));
    stack.PutLong(cfa1 - 4, 0);

    UnwindInputs in = MakeInputs(stack, kLoadBase + kDisableCache + 10, sp0);
    const UnwindResult r = StackUnwinder::Unwind(in);

    ASSERT_EQ(r.frames.size(), 2u);
    EXPECT_EQ(r.frames[1].name, "voxel_c2p_kick"); // not whatever starts at 0x2a30
    EXPECT_EQ(r.frames[1].pc, retAtBoundary);      // but the real return address is displayed
}
