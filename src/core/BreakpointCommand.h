#pragma once

// Pure, header-only builders for the forked-UAE debugger breakpoint console commands.
//
// Kept free of ImGui / view dependencies (like BlitScreenMapper.h) so the syntax-sensitive
// command strings can be unit-tested against adm_core without linking the executable target.
//
// Conditional / counted PC breakpoints are driven through UAE's native explicit-node command
// `fo <idx> <reg> <oper> <value1> [<mask> [<value2>]] [N<count>] [H<chain>]`
// (instruction_breakpoint() / check_breakpoint_extra() in debug.c). The optional N/H tokens are
// parsed only AFTER the positional mask/value2 reads, so a PC head node that wants N=/H= must
// supply an explicit mask + value2 first — UAE ignores them for a PC-type node (it compares
// value1 == pc directly), but their presence keeps the greedy positional parser from swallowing
// the N/H tokens. The count/chain are read with readint() (decimal); value1/mask are readhex().

#include <cstdint>
#include <string>
#include <vector>

#include <fmt/format.h>

namespace adm::core {

// Build the bpnode command(s) for a conditional and/or counted PC breakpoint.
//
//  addr8       : normalised 8-hex-digit PC address (e.g. "00FC00A0").
//  bpSlot      : bpnode index for the PC head node (0..BREAKPOINT_TOTAL-1).
//  condEnabled : attach a register-compare condition (chained node).
//  condReg     : UAE register token (D0..D7, A0..A7, PC, SR, USP, ...).
//  condOper    : UAE operator token (==, !=, <, >, <=, >=).
//  condValue   : compared value (hex).
//  condMask    : AND-mask applied before compare, i.e. (reg & mask) <oper> value.
//  condSlot    : bpnode index for the chained condition node (used iff condEnabled).
//  ignoreCount : skip this many hits before breaking (UAE N=, 0 = break immediately).
//
// Returns the commands in apply order: condition node first (so the chain target exists), then
// the chained PC head node. Returns an empty vector when no explicit node is needed (caller
// should fall back to the plain "f <addr>" toggle in that case).
inline std::vector<std::string>
BuildSmartBreakpointAddCommands(const std::string& addr8, int bpSlot, bool condEnabled,
                                const std::string& condReg, const std::string& condOper,
                                uint32_t condValue, uint32_t condMask, int condSlot,
                                int ignoreCount)
{
    std::vector<std::string> cmds;
    if (!condEnabled && ignoreCount <= 0)
        return cmds; // plain breakpoint: caller uses "f <addr>"

    if (condEnabled && condSlot >= 0) {
        // fo <condSlot> <reg> <oper> <value> <mask>
        cmds.push_back(
            fmt::format("fo {} {} {} {:X} {:X}", condSlot, condReg, condOper, condValue, condMask));
    }

    // fo <bpSlot> PC == <addr> <mask=FFFFFFFF> <value2=0> [N<count>] [H<condSlot>]
    std::string head = fmt::format("fo {} PC == {} FFFFFFFF 0", bpSlot, addr8);
    if (ignoreCount > 0)
        head += fmt::format(" N{}", ignoreCount);
    if (condEnabled && condSlot >= 0)
        head += fmt::format(" H{}", condSlot);
    cmds.push_back(std::move(head));
    return cmds;
}

// Build the bpnode removal command(s) for a smart PC breakpoint: `fr <idx>` disables a node.
// Removes the chained condition node (if any) and the PC head node.
inline std::vector<std::string> BuildSmartBreakpointRemoveCommands(int bpSlot, bool condEnabled,
                                                                   int condSlot)
{
    std::vector<std::string> cmds;
    if (condEnabled && condSlot >= 0)
        cmds.push_back(fmt::format("fr {}", condSlot));
    if (bpSlot >= 0)
        cmds.push_back(fmt::format("fr {}", bpSlot));
    return cmds;
}

} // namespace adm::core
