// Tests for the syntax-sensitive UAE breakpoint console command builder. The forked-UAE
// instruction_breakpoint()/check_breakpoint_extra() parser is greedy on the optional
// mask/value2 positional reads, so the PC head node must emit an explicit mask + value2 before
// any N/H tokens. These tests pin the exact strings.

#include "core/BreakpointCommand.h"

#include <gtest/gtest.h>

using adm::core::BuildSmartBreakpointAddCommands;
using adm::core::BuildSmartBreakpointRemoveCommands;

TEST(BreakpointCommand, PlainBreakpointEmitsNoExplicitNode)
{
    auto cmds = BuildSmartBreakpointAddCommands("00FC00A0", 19, /*condEnabled=*/false, "D0",
                                                "==", 0, 0xFFFFFFFF, -1, /*ignoreCount=*/0);
    EXPECT_TRUE(cmds.empty());
}

TEST(BreakpointCommand, ConditionOnlyEmitsConditionThenChainedHead)
{
    auto cmds = BuildSmartBreakpointAddCommands("00FC00A0", 19, /*condEnabled=*/true, "D0",
                                                "==", 0x1234, 0xFFFFFFFF, 18, /*ignoreCount=*/0);
    ASSERT_EQ(cmds.size(), 2u);
    EXPECT_EQ(cmds[0], "fo 18 D0 == 1234 FFFFFFFF");
    EXPECT_EQ(cmds[1], "fo 19 PC == 00FC00A0 FFFFFFFF 0 H18");
}

TEST(BreakpointCommand, IgnoreCountOnlyEmitsHeadWithN)
{
    auto cmds = BuildSmartBreakpointAddCommands("00040000", 19, /*condEnabled=*/false, "D0",
                                                "==", 0, 0xFFFFFFFF, -1, /*ignoreCount=*/5);
    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0], "fo 19 PC == 00040000 FFFFFFFF 0 N5");
}

TEST(BreakpointCommand, ConditionAndCountEmitBoth)
{
    auto cmds = BuildSmartBreakpointAddCommands("00040000", 19, /*condEnabled=*/true, "A6",
                                                ">=", 0xC00000, 0xFFFFFFFF, 18, /*ignoreCount=*/5);
    ASSERT_EQ(cmds.size(), 2u);
    EXPECT_EQ(cmds[0], "fo 18 A6 >= C00000 FFFFFFFF");
    EXPECT_EQ(cmds[1], "fo 19 PC == 00040000 FFFFFFFF 0 N5 H18");
}

TEST(BreakpointCommand, MaskedConditionEmitsMask)
{
    auto cmds = BuildSmartBreakpointAddCommands("00040000", 19, /*condEnabled=*/true, "D7", "==", 3,
                                                0xFF, 18, /*ignoreCount=*/0);
    ASSERT_EQ(cmds.size(), 2u);
    EXPECT_EQ(cmds[0], "fo 18 D7 == 3 FF");
    EXPECT_EQ(cmds[1], "fo 19 PC == 00040000 FFFFFFFF 0 H18");
}

TEST(BreakpointCommand, RemoveEmitsConditionThenHead)
{
    auto cmds = BuildSmartBreakpointRemoveCommands(19, /*condEnabled=*/true, 18);
    ASSERT_EQ(cmds.size(), 2u);
    EXPECT_EQ(cmds[0], "fr 18");
    EXPECT_EQ(cmds[1], "fr 19");
}

TEST(BreakpointCommand, RemoveCountOnlyEmitsHeadOnly)
{
    auto cmds = BuildSmartBreakpointRemoveCommands(19, /*condEnabled=*/false, -1);
    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0], "fr 19");
}
