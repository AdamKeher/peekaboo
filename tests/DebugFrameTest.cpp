#include <gtest/gtest.h>

#include "core/DebugFrame.h"

#include <filesystem>
#include <string>

using adm::core::CfiRegRuleKind;
using adm::core::CfiRow;
using adm::core::DebugFrame;
using adm::core::kDwarfRegPc;
using adm::core::kDwarfRegSp;

namespace {

std::string FixturePath()
{
    // Same discovery the other ELF-backed tests use: the fixture is a real bartman
    // toolchain build (a.elf) checked in under tests/images.
    for (const char* candidate : {"tests/images/a.elf", "../tests/images/a.elf",
                                  "../../tests/images/a.elf", "../../../tests/images/a.elf"}) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

class DebugFrameTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const std::string path = FixturePath();
        if (path.empty()) {
            GTEST_SKIP() << "tests/images/a.elf fixture not found";
        }
        ASSERT_TRUE(m_frame.LoadFromElf(path)) << "failed to parse .debug_frame from " << path;
    }

    DebugFrame m_frame;
};

// system_disable_cache in the fixture's link map. Its FDE is the worked example in the
// plan: 84 bytes long, with the CFA moving five times inside the one function.
constexpr uint32_t kDisableCache = 0x2498;

// __mulsi3, a hand-written libgcc asm helper. It comes from a separately assembled
// object whose CFI uses the *other*, DWARF-version-1 CIE in this ELF.
constexpr uint32_t kMulsi3 = 0x4654;

// _doynaxdepack_asm: 210 bytes of hand-written assembly at the very end of .text with no
// FDE at all. Real code, genuinely outside CFI coverage -- which is the case the unwinder
// has to stop on and explain rather than guess past.
constexpr uint32_t kUncoveredAsm = 0x4760;

} // namespace

TEST_F(DebugFrameTest, LoadsEveryFdeAndCie)
{
    // Ground truth from decoding .debug_frame directly: 2 CIEs, 35 FDEs.
    EXPECT_EQ(m_frame.FdeCount(), 35u);
    EXPECT_EQ(m_frame.CieCount(), 2u);
    EXPECT_TRUE(m_frame.IsLoaded());
}

TEST_F(DebugFrameTest, CieInitialStateIsReturnAddressJustPushed)
{
    // At the very first instruction of a function, a7 points straight at the return
    // address the jsr pushed: CFA = a7 + 4, and the return address sits at CFA - 4.
    CfiRow row;
    ASSERT_TRUE(m_frame.ResolveRow(kDisableCache, row));
    EXPECT_TRUE(row.cfaValid);
    EXPECT_EQ(row.cfaReg, kDwarfRegSp);
    EXPECT_EQ(row.cfaOffset, 4);
    EXPECT_EQ(row.raRegister, kDwarfRegPc);
    EXPECT_EQ(row.reg[kDwarfRegPc].kind, CfiRegRuleKind::OffsetCfa);
    EXPECT_EQ(row.reg[kDwarfRegPc].offset, -4);
}

TEST_F(DebugFrameTest, CfaTracksThePrologueAndEpilogue)
{
    // The whole reason CFI is needed rather than an a6 chain walk: within one function
    // the offset from a7 back to the frame anchor changes five times. Decoded from
    // .debug_frame with an independent parser.
    struct Expect
    {
        uint32_t offset;
        int32_t cfa;
    };
    const Expect points[] = {
        {0, 4}, {4, 16}, {6, 20}, {78, 16}, {82, 4},
    };

    for (const Expect& p : points) {
        CfiRow row;
        ASSERT_TRUE(m_frame.ResolveRow(kDisableCache + p.offset, row)) << "at +" << p.offset;
        EXPECT_EQ(row.cfaReg, kDwarfRegSp) << "at +" << p.offset;
        EXPECT_EQ(row.cfaOffset, p.cfa) << "at +" << p.offset;
        // The return-address rule holds for the whole function.
        EXPECT_EQ(row.reg[kDwarfRegPc].kind, CfiRegRuleKind::OffsetCfa) << "at +" << p.offset;
        EXPECT_EQ(row.reg[kDwarfRegPc].offset, -4) << "at +" << p.offset;
    }
}

TEST_F(DebugFrameTest, RecordsCalleeSavedRegisterSlot)
{
    // a6 (DWARF register 14) is saved at CFA-20 from +6 onwards. Note it is an ordinary
    // callee-saved register here, not a frame pointer.
    constexpr uint32_t kA6 = 14;

    CfiRow before;
    ASSERT_TRUE(m_frame.ResolveRow(kDisableCache + 4, before));
    EXPECT_EQ(before.reg[kA6].kind, CfiRegRuleKind::Undefined);

    CfiRow after;
    ASSERT_TRUE(m_frame.ResolveRow(kDisableCache + 6, after));
    EXPECT_EQ(after.reg[kA6].kind, CfiRegRuleKind::OffsetCfa);
    EXPECT_EQ(after.reg[kA6].offset, -20);
}

TEST_F(DebugFrameTest, CoversReportsFdeExtent)
{
    EXPECT_TRUE(m_frame.Covers(kDisableCache));
    EXPECT_TRUE(m_frame.Covers(kDisableCache + 83)); // last byte of the 84-byte range

    // Compiled functions abut, so one past the end is covered -- by the *next* FDE, not
    // by this one. The rules have to come from the right entry: a6 is saved in
    // system_disable_cache but has no rule yet at the entry of the function after it.
    constexpr uint32_t kA6 = 14;
    CfiRow inside;
    ASSERT_TRUE(m_frame.ResolveRow(kDisableCache + 40, inside));
    EXPECT_EQ(inside.reg[kA6].kind, CfiRegRuleKind::OffsetCfa);

    ASSERT_TRUE(m_frame.Covers(kDisableCache + 84));
    CfiRow nextFn;
    ASSERT_TRUE(m_frame.ResolveRow(kDisableCache + 84, nextFn));
    EXPECT_EQ(nextFn.cfaOffset, 4); // entry state of the following function
    EXPECT_EQ(nextFn.reg[kA6].kind, CfiRegRuleKind::Undefined);

    // Genuinely uncovered: hand-written asm with no FDE, and a Kickstart address that is
    // not part of this ELF at all.
    CfiRow row;
    EXPECT_FALSE(m_frame.Covers(kUncoveredAsm));
    EXPECT_FALSE(m_frame.ResolveRow(kUncoveredAsm, row));
    EXPECT_FALSE(m_frame.Covers(0x00FC0000));
    EXPECT_FALSE(m_frame.ResolveRow(0x00FC0000, row));
}

TEST_F(DebugFrameTest, ParsesTheVersion1Cie)
{
    // The fixture carries two CIEs with different encodings: version 3 (code align 1,
    // data align -2) covering the compiled C, and version 1 (code align 2, data align -4)
    // covering seven hand-written libgcc asm helpers. Version 1 stores the return-address
    // register as a raw byte instead of LEB128, so mis-handling it would throw the operand
    // stream off by a byte for every FDE underneath it.
    //
    // __mulsi3 never touches a7, so its FDE carries no instructions at all and the row is
    // purely the CIE's initial state -- which makes it a direct read of the CIE fields.
    CfiRow row;
    ASSERT_TRUE(m_frame.ResolveRow(kMulsi3, row));
    EXPECT_EQ(row.raRegister, kDwarfRegPc);
    EXPECT_EQ(row.cfaReg, kDwarfRegSp);
    EXPECT_EQ(row.cfaOffset, 4);
    EXPECT_EQ(row.reg[kDwarfRegPc].kind, CfiRegRuleKind::OffsetCfa);
    EXPECT_EQ(row.reg[kDwarfRegPc].offset, -4);

    EXPECT_TRUE(m_frame.Covers(kMulsi3 + 33)); // 34-byte function
}

TEST_F(DebugFrameTest, ResolvesInsideTheLargeLtoFunction)
{
    // The biggest FDE in the fixture is main: 9234 bytes starting at 0x84, with a long
    // run of def_cfa_offset changes. Resolving deep inside it exercises a real opcode
    // stream rather than the first few instructions.
    CfiRow row;
    ASSERT_TRUE(m_frame.ResolveRow(0x84 + 4000, row));
    EXPECT_TRUE(row.cfaValid);
    EXPECT_EQ(row.cfaReg, kDwarfRegSp);
    EXPECT_GT(row.cfaOffset, 0);
    EXPECT_EQ(row.reg[kDwarfRegPc].kind, CfiRegRuleKind::OffsetCfa);
    EXPECT_EQ(row.reg[kDwarfRegPc].offset, -4);
}

TEST(DebugFrameStandaloneTest, RejectsNonElfInput)
{
    DebugFrame frame;
    const uint8_t junk[] = {'n', 'o', 't', ' ', 'e', 'l', 'f'};
    EXPECT_FALSE(frame.LoadFromElf(std::span<const uint8_t>(junk, sizeof(junk))));
    EXPECT_FALSE(frame.IsLoaded());
    EXPECT_EQ(frame.FdeCount(), 0u);

    CfiRow row;
    EXPECT_FALSE(frame.ResolveRow(0x1000, row));
}
