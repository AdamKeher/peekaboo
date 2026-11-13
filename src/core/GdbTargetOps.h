#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "WatchpointTypes.h"

namespace adm::core {

/**
 * @brief The 68000 register file as the GDB remote protocol wants it.
 *
 * a[7] is the stack pointer: GDB's m68k description names register 15 `sp`, and there is no
 * separate a7. `sr` is 16-bit on the hardware but occupies a full 32-bit slot in the `g` packet.
 */
struct GdbRegs
{
    uint32_t d[8] = {};
    uint32_t a[8] = {}; // a[7] == sp
    uint32_t sr = 0;
    uint32_t pc = 0;
};

/**
 * @brief Everything GdbPacketHandler is allowed to do to the machine.
 *
 * A bag of optional std::function hooks, deliberately shaped like McpHost / ArHost: a null hook
 * means "this target cannot do that", and the handler answers with an RSP error rather than
 * crashing. Keeping the surface this narrow is what lets GdbPacketHandler stay pure and be
 * unit-tested against a fake target with no emulator, no sockets and no threads.
 *
 * Unless noted, hooks are invoked on whichever thread is servicing the packet. Read-only hooks
 * (readRegs / readMem) may be called from the socket thread while the target is halted, because
 * nothing is mutating machine state then; state-mutating hooks are only ever called from the
 * main thread. See GdbServer for the split.
 */
struct GdbTargetOps
{
    // --- state queries -------------------------------------------------------------------
    std::function<bool()> isRunning; // a core is loaded and a program is running
    std::function<bool()> isHalted;  // the emulator is paused (GDB's notion of "stopped")

    // --- registers -----------------------------------------------------------------------
    std::function<bool(GdbRegs&)> readRegs;
    // regIndex uses GDB's numbering: 0-7 = d0-d7, 8-14 = a0-a6, 15 = sp, 16 = sr, 17 = pc.
    std::function<bool(int regIndex, uint32_t value)> writeReg;

    // --- memory --------------------------------------------------------------------------
    // Reads up to len bytes into out. `valid`, when non-null, is resized to len and marks which
    // bytes are backed by real memory: unmapped bytes must be reported, never filled in, so an
    // `m` packet over an unmapped range can answer E01 instead of inventing zeroes.
    std::function<size_t(uint32_t addr, uint8_t* out, size_t len, std::vector<bool>* valid)>
        readMem;
    std::function<bool(uint32_t addr, const uint8_t* data, size_t len, std::string& err)> writeMem;

    // --- breakpoints ---------------------------------------------------------------------
    // Z0/z0. Backed by UAE bpnodes, which halt BEFORE the instruction at the address executes --
    // exactly Z0's semantics -- and which show up in Peekaboo's own Breakpoints window, so a GDB
    // breakpoint cannot desync from the UI.
    std::function<bool(uint32_t addr)> addBreakpoint;
    std::function<bool(uint32_t addr)> removeBreakpoint;

    // Z2/Z3/Z4. Returns the slot the watchpoint occupies, or -1 with a reason in outError; UAE
    // has a fixed number of slots, and running out must be reported rather than silently dropped.
    std::function<int(const WatchpointSpec& spec, std::string& outError)> addWatchpoint;
    std::function<bool(int slot)> removeWatchpoint;

    // --- execution -----------------------------------------------------------------------
    // Resume. The stop reply is delivered later and asynchronously, when the machine actually
    // stops -- GdbServer::NotifyStopped() does that from the debug-break callback.
    std::function<void()> cont;
    // One instruction. Synchronous: when this returns the machine is stopped again, so the
    // caller replies immediately rather than waiting for a notification.
    std::function<void()> stepOne;
    // Halt a running target (GDB's Ctrl-C / vCont;t).
    std::function<void()> interrupt;
    /**
     * @brief vCont;r -- keep stepping while the PC stays inside [lo, hi).
     *
     * Worth having rather than letting GDB step one instruction at a time: on -Ofast code a
     * single source-level `next` can cover thousands of instructions, and each one would
     * otherwise cost a full packet round-trip. Returns true if the target is now stopped inside
     * the caller's expectations, false if it should fall back to plain stepping.
     */
    std::function<bool(uint32_t lo, uint32_t hi)> stepRange;

    // --- reverse execution -----------------------------------------------------------------
    // `bs` and `bc`. Both return false when history has run out, with a reason in outError so
    // "cannot go back further" reads differently from "this went wrong". With these two, GDB
    // provides reverse-step, reverse-next and reverse-finish on top, DWARF-aware.
    std::function<bool(std::string& outError)> reverseStep;
    std::function<bool(std::string& outError)> reverseContinue;
    // True when reverse execution is possible at all right now.
    std::function<bool()> canReverse;

    // --- monitor / passthrough -----------------------------------------------------------
    // qRcmd ("monitor <cmd>"). Returns text to hand back to the user.
    std::function<std::string(const std::string& command)> monitor;
};

} // namespace adm::core
