#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "../src/core/GdbPacketHandler.h"
#include "../src/core/GdbTargetOps.h"

using adm::core::GdbPacketHandler;
using adm::core::GdbRegs;
using adm::core::GdbTargetOps;

namespace {

// A fake target: a flat address space with an explicitly mapped window, so "unmapped" is a real
// state the handler has to cope with rather than an abstraction we cannot reach from a test.
struct FakeTarget
{
    static constexpr uint32_t kBase = 0x00C00000;
    static constexpr uint32_t kSize = 0x100;

    static constexpr size_t kMaxBreakpoints = 3; // small, so exhaustion is reachable in a test
    static constexpr size_t kMaxWatchpoints = 2;

    GdbRegs regs;
    std::vector<uint8_t> mem = std::vector<uint8_t>(kSize, 0);
    std::vector<std::pair<int, uint32_t>> regWrites;
    std::string lastMonitor;
    std::vector<uint32_t> breakpoints;
    std::vector<adm::core::WatchpointSpec> watchpoints;
    std::vector<int> removedSlots;
    std::vector<std::pair<uint32_t, uint32_t>> rangeSteps;
    bool rangeStepSucceeds = true;
    int resumes = 0, steps = 0, interrupts = 0;

    GdbTargetOps Ops()
    {
        GdbTargetOps ops;
        ops.isRunning = [] { return true; };
        ops.isHalted = [] { return true; };
        ops.readRegs = [this](GdbRegs& out) {
            out = regs;
            return true;
        };
        ops.writeReg = [this](int idx, uint32_t v) {
            regWrites.emplace_back(idx, v);
            if (idx >= 0 && idx < 8)
                regs.d[idx] = v;
            else if (idx >= 8 && idx < 16)
                regs.a[idx - 8] = v;
            else if (idx == 16)
                regs.sr = v;
            else if (idx == 17)
                regs.pc = v;
            return true;
        };
        ops.readMem = [this](uint32_t addr, uint8_t* out, size_t len, std::vector<bool>* valid) {
            if (valid)
                valid->assign(len, false);
            for (size_t i = 0; i < len; ++i) {
                const uint32_t a = addr + static_cast<uint32_t>(i);
                if (a >= kBase && a < kBase + kSize) {
                    out[i] = mem[a - kBase];
                    if (valid)
                        (*valid)[i] = true;
                } else {
                    out[i] = 0;
                }
            }
            return len;
        };
        ops.writeMem = [this](uint32_t addr, const uint8_t* data, size_t len, std::string& err) {
            if (addr < kBase || addr + len > kBase + kSize) {
                err = "unmapped";
                return false;
            }
            std::memcpy(mem.data() + (addr - kBase), data, len);
            return true;
        };
        ops.monitor = [this](const std::string& cmd) {
            lastMonitor = cmd;
            return "ack:" + cmd;
        };
        ops.addBreakpoint = [this](uint32_t addr) {
            if (breakpoints.size() >= kMaxBreakpoints)
                return false; // UAE has a fixed bpnode count
            breakpoints.push_back(addr);
            return true;
        };
        ops.removeBreakpoint = [this](uint32_t addr) {
            for (auto it = breakpoints.begin(); it != breakpoints.end(); ++it)
                if (*it == addr) {
                    breakpoints.erase(it);
                    return true;
                }
            return false;
        };
        ops.addWatchpoint = [this](const adm::core::WatchpointSpec& spec, std::string& err) {
            if (watchpoints.size() >= kMaxWatchpoints) {
                err = "no free slots";
                return -1;
            }
            watchpoints.push_back(spec);
            return static_cast<int>(watchpoints.size()) - 1;
        };
        ops.removeWatchpoint = [this](int slot) {
            removedSlots.push_back(slot);
            return slot >= 0 && slot < static_cast<int>(watchpoints.size());
        };
        ops.cont = [this]() { ++resumes; };
        ops.stepOne = [this]() { ++steps; };
        ops.interrupt = [this]() { ++interrupts; };
        ops.stepRange = [this](uint32_t lo, uint32_t hi) {
            rangeSteps.emplace_back(lo, hi);
            return rangeStepSucceeds;
        };
        return ops;
    }
};

std::string Handle(GdbPacketHandler& h, const GdbTargetOps& ops, const std::string& packet)
{
    return h.Handle(packet, ops).payload;
}

} // namespace

// --- register packet -----------------------------------------------------------------------

TEST(GdbPacketHandlerTest, GPacketIsGdbsM68kLayout)
{
    FakeTarget t;
    for (int i = 0; i < 8; ++i)
        t.regs.d[i] = 0x11110000u + static_cast<uint32_t>(i);
    for (int i = 0; i < 8; ++i)
        t.regs.a[i] = 0x22220000u + static_cast<uint32_t>(i);
    t.regs.sr = 0x2700;
    t.regs.pc = 0x00C0CB68;

    GdbPacketHandler h;
    auto ops = t.Ops();
    const std::string g = Handle(h, ops, "g");

    // 29 registers / 180 bytes, per GDB's built-in m68k description.
    ASSERT_EQ(g.size(), GdbPacketHandler::kGPacketBytes * 2);
    EXPECT_EQ(g.substr(0, 8), "11110000");   // d0
    EXPECT_EQ(g.substr(56, 8), "11110007");  // d7 at byte 28
    EXPECT_EQ(g.substr(64, 8), "22220000");  // a0 at byte 32
    EXPECT_EQ(g.substr(112, 8), "22220006"); // a6 at byte 56
    EXPECT_EQ(g.substr(120, 8), "22220007"); // sp (a7) at byte 60
    EXPECT_EQ(g.substr(128, 8), "00002700"); // sr at byte 64, zero-extended
    EXPECT_EQ(g.substr(136, 8), "00c0cb68"); // pc at byte 68

    // The FPU half must be 'x' (unavailable), never fabricated zeroes.
    const std::string fpu = g.substr(GdbPacketHandler::kIntRegBytes * 2);
    EXPECT_EQ(fpu.size(), (GdbPacketHandler::kGPacketBytes - GdbPacketHandler::kIntRegBytes) * 2);
    EXPECT_EQ(fpu.find_first_not_of('x'), std::string::npos);
}

TEST(GdbPacketHandlerTest, ReadSingleRegister)
{
    FakeTarget t;
    t.regs.pc = 0xDEADBEEF;
    t.regs.a[7] = 0x00C24A04;
    GdbPacketHandler h;
    auto ops = t.Ops();

    EXPECT_EQ(Handle(h, ops, "p11"), "deadbeef"); // 0x11 = 17 = pc
    EXPECT_EQ(Handle(h, ops, "pf"), "00c24a04");  // 0x0f = 15 = sp

    // An FPU register is unavailable, not zero: fp0 is 12 bytes, fpcontrol 4.
    EXPECT_EQ(Handle(h, ops, "p12"), std::string(24, 'x'));
    EXPECT_EQ(Handle(h, ops, "p1a"), std::string(8, 'x'));
    // Out of range entirely.
    EXPECT_EQ(Handle(h, ops, "p1d"), "E16");
}

TEST(GdbPacketHandlerTest, WriteSingleRegister)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();

    EXPECT_EQ(Handle(h, ops, "P0=12345678"), "OK");
    EXPECT_EQ(t.regs.d[0], 0x12345678u);
    EXPECT_EQ(Handle(h, ops, "P11=00c00100"), "OK");
    EXPECT_EQ(t.regs.pc, 0x00C00100u);
    // FPU writes are refused rather than silently dropped.
    EXPECT_EQ(Handle(h, ops, "P12=00000000"), "E01");
}

TEST(GdbPacketHandlerTest, WriteAllRegistersSkipsUnavailableFields)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();

    GdbRegs desired;
    for (int i = 0; i < 8; ++i)
        desired.d[i] = 0xA0000000u + static_cast<uint32_t>(i);
    for (int i = 0; i < 8; ++i)
        desired.a[i] = 0xB0000000u + static_cast<uint32_t>(i);
    desired.sr = 0x2000;
    desired.pc = 0x00C01234;

    EXPECT_EQ(Handle(h, ops, "G" + GdbPacketHandler::FormatGPacket(desired)), "OK");
    EXPECT_EQ(t.regs.d[3], 0xA0000003u);
    EXPECT_EQ(t.regs.a[7], 0xB0000007u);
    EXPECT_EQ(t.regs.pc, 0x00C01234u);
    // The 'x' FPU tail must not have produced writes for registers 18+.
    for (const auto& [idx, value] : t.regWrites)
        EXPECT_LT(idx, GdbPacketHandler::kIntRegCount);
}

// --- memory --------------------------------------------------------------------------------

TEST(GdbPacketHandlerTest, ReadMappedMemory)
{
    FakeTarget t;
    for (int i = 0; i < 8; ++i)
        t.mem[i] = static_cast<uint8_t>(0x10 + i);
    GdbPacketHandler h;
    auto ops = t.Ops();

    EXPECT_EQ(Handle(h, ops, "mC00000,8"), "1011121314151617");
}

TEST(GdbPacketHandlerTest, ReadPastRegionEndIsE01NotFiller)
{
    // The whole point: an unmapped byte must surface as an error so GDB says "Cannot access
    // memory", rather than being filled with a plausible-looking zero.
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();

    EXPECT_EQ(Handle(h, ops, "mBFFFFF,4"), "E01"); // straddles the start of the region
    EXPECT_EQ(Handle(h, ops, "mC000FE,4"), "E01"); // straddles the end
    EXPECT_EQ(Handle(h, ops, "mF00000,4"), "E01"); // nowhere near it
}

TEST(GdbPacketHandlerTest, WriteMemoryHexAndBinary)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();

    EXPECT_EQ(Handle(h, ops, "MC00000,4:deadbeef"), "OK");
    EXPECT_EQ(t.mem[0], 0xDE);
    EXPECT_EQ(t.mem[3], 0xEF);

    // X with escaped bytes: '}' introduces the next byte XOR 0x20.
    std::string x = "XC00010,4:";
    x += static_cast<char>(0x01);
    x += '}';
    x += static_cast<char>('#' ^ 0x20); // escaped '#'
    x += '}';
    x += static_cast<char>('$' ^ 0x20); // escaped '$'
    x += static_cast<char>(0x02);
    EXPECT_EQ(Handle(h, ops, x), "OK");
    EXPECT_EQ(t.mem[0x10], 0x01);
    EXPECT_EQ(t.mem[0x11], '#');
    EXPECT_EQ(t.mem[0x12], '$');
    EXPECT_EQ(t.mem[0x13], 0x02);

    // Writing outside the mapped window must fail, not silently succeed.
    EXPECT_EQ(Handle(h, ops, "MF00000,2:1234"), "E01");
    // A length that disagrees with the payload is malformed.
    EXPECT_EQ(Handle(h, ops, "MC00000,4:1234"), "E16");
}

TEST(GdbPacketHandlerTest, ZeroLengthBinaryWriteIsTheProbe)
{
    // GDB sends "X<addr>,0:" to discover whether binary writes work at all.
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    EXPECT_EQ(Handle(h, ops, "XC00000,0:"), "OK");
}

// --- encoding helpers ------------------------------------------------------------------------

TEST(GdbPacketHandlerTest, BinaryDecodeHandlesRunLength)
{
    std::vector<uint8_t> out;
    // 'A' followed by *<29+4> repeats it four more times.
    std::string in = "A*";
    in += static_cast<char>(29 + 4);
    ASSERT_TRUE(GdbPacketHandler::DecodeBinary(in, out));
    EXPECT_EQ(out.size(), 5u);
    for (uint8_t b : out)
        EXPECT_EQ(b, 'A');

    // A trailing escape with nothing after it is malformed.
    EXPECT_FALSE(GdbPacketHandler::DecodeBinary("ab}", out));
    // A run with no preceding byte is malformed.
    std::string bad = "*";
    bad += static_cast<char>(29 + 4);
    EXPECT_FALSE(GdbPacketHandler::DecodeBinary(bad, out));
}

TEST(GdbPacketHandlerTest, RunLengthEncodeRoundTripsAndShrinks)
{
    const std::string zeros(200, '0');
    const std::string encoded = GdbPacketHandler::RunLengthEncode(zeros);
    EXPECT_LT(encoded.size(), zeros.size());

    std::vector<uint8_t> decoded;
    ASSERT_TRUE(GdbPacketHandler::DecodeBinary(encoded, decoded));
    ASSERT_EQ(decoded.size(), zeros.size());
    EXPECT_EQ(std::string(decoded.begin(), decoded.end()), zeros);

    // Never emit '#' or '$' as a count byte - they would terminate the packet early.
    for (size_t i = 0; i + 1 < encoded.size(); ++i) {
        if (encoded[i] == '*')
            EXPECT_TRUE(encoded[i + 1] != '#' && encoded[i + 1] != '$');
    }

    // Short runs are left alone rather than expanded.
    EXPECT_EQ(GdbPacketHandler::RunLengthEncode("abc"), "abc");
}

TEST(GdbPacketHandlerTest, FromHexRejectsMalformedInput)
{
    std::vector<uint8_t> out;
    EXPECT_TRUE(GdbPacketHandler::FromHex("00ff10", out));
    EXPECT_EQ(out.size(), 3u);
    EXPECT_FALSE(GdbPacketHandler::FromHex("abc", out)); // odd length
    EXPECT_FALSE(GdbPacketHandler::FromHex("zz", out));  // not hex
}

// --- handshake -------------------------------------------------------------------------------

TEST(GdbPacketHandlerTest, SupportedAdvertisesNoTargetXml)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    const std::string caps = Handle(h, ops, "qSupported:multiprocess+;swbreak+");

    EXPECT_NE(caps.find("PacketSize=4000"), std::string::npos);
    EXPECT_NE(caps.find("QStartNoAckMode+"), std::string::npos);
    EXPECT_NE(caps.find("swbreak+"), std::string::npos);
    // We serve GDB's built-in m68k layout; advertising target.xml support would be a lie and
    // would drag in the expat dependency Bartman's gdb needs.
    EXPECT_NE(caps.find("qXfer:features:read-"), std::string::npos);
}

TEST(GdbPacketHandlerTest, NoAckModeLatchesAfterOk)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    EXPECT_FALSE(h.NoAckMode());
    EXPECT_EQ(Handle(h, ops, "QStartNoAckMode"), "OK");
    EXPECT_TRUE(h.NoAckMode());
    h.Reset();
    EXPECT_FALSE(h.NoAckMode());
}

TEST(GdbPacketHandlerTest, StopReplyAndThreadQueries)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    EXPECT_EQ(Handle(h, ops, "?"), "T05thread:1;");
    EXPECT_EQ(Handle(h, ops, "qC"), "QC1");
    EXPECT_EQ(Handle(h, ops, "qfThreadInfo"), "m1");
    EXPECT_EQ(Handle(h, ops, "qsThreadInfo"), "l");
    EXPECT_EQ(Handle(h, ops, "qAttached"), "1");
    EXPECT_EQ(Handle(h, ops, "Hg0"), "OK");
    EXPECT_EQ(Handle(h, ops, "Hc-1"), "OK");
}

TEST(GdbPacketHandlerTest, MonitorCommandRoundTrip)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    // "sections" hex-encoded.
    const std::string reply = Handle(h, ops, "qRcmd,73656374696f6e73");
    EXPECT_EQ(t.lastMonitor, "sections");
    std::vector<uint8_t> decoded;
    ASSERT_TRUE(GdbPacketHandler::FromHex(reply, decoded));
    EXPECT_EQ(std::string(decoded.begin(), decoded.end()), "ack:sections");
}

TEST(GdbPacketHandlerTest, DetachAndKillCloseTheConnection)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    for (const char* p : {"D", "k", "vKill;1"}) {
        auto r = h.Handle(p, ops);
        EXPECT_EQ(r.payload, "OK");
        EXPECT_TRUE(r.closeConnection) << p;
    }
}

TEST(GdbPacketHandlerTest, UnknownPacketsGetAnEmptyReplyNotSilence)
{
    // RSP requires an empty reply for unimplemented packets. Staying silent hangs GDB.
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    // Genuinely unimplemented packets. vCont;c and z... are deliberately absent: both are
    // implemented now, and a malformed z is an error rather than an unknown packet.
    for (const char* p : {"qTStatus", "qWibble", "Y", ""}) {
        auto r = h.Handle(p, ops);
        EXPECT_TRUE(r.hasReply) << p;
        EXPECT_TRUE(r.payload.empty()) << p;
    }
}

TEST(GdbPacketHandlerTest, MissingHooksReportErrorsRatherThanCrashing)
{
    GdbTargetOps empty; // every hook null
    GdbPacketHandler h;
    EXPECT_EQ(h.Handle("g", empty).payload, "E01");
    EXPECT_EQ(h.Handle("mC00000,4", empty).payload, "E01");
    EXPECT_EQ(h.Handle("MC00000,2:1234", empty).payload, "E01");
    EXPECT_EQ(h.Handle("P0=00000001", empty).payload, "E01");
    // qRcmd with no monitor hook is "unsupported", which is different from an error.
    auto r = h.Handle("qRcmd,73656374696f6e73", empty);
    EXPECT_TRUE(r.payload.empty());
}

TEST(GdbPacketHandlerTest, MalformedAddressesAreRejected)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    EXPECT_EQ(Handle(h, ops, "m"), "E16");
    EXPECT_EQ(Handle(h, ops, "mC00000"), "E16");   // no length
    EXPECT_EQ(Handle(h, ops, "mzzz,4"), "E16");    // not hex
    EXPECT_EQ(Handle(h, ops, "MC00000,4"), "E16"); // no colon
}

// --- run control (stage 3) -------------------------------------------------------------------

TEST(GdbPacketHandlerTest, SoftwareAndHardwareBreakpointsBothUseBpnodes)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();

    EXPECT_EQ(Handle(h, ops, "Z0,c0cb68,2"), "OK");
    EXPECT_EQ(Handle(h, ops, "Z1,c0cb80,2"), "OK"); // hardware: same mechanism here
    ASSERT_EQ(t.breakpoints.size(), 2u);
    EXPECT_EQ(t.breakpoints[0], 0x00C0CB68u);

    EXPECT_EQ(Handle(h, ops, "z0,c0cb68,2"), "OK");
    EXPECT_EQ(t.breakpoints.size(), 1u);
    // Removing one that was never set is an error, not a silent success.
    EXPECT_EQ(Handle(h, ops, "z0,deadbe,2"), "E22");
}

TEST(GdbPacketHandlerTest, BreakpointExhaustionIsReportedNotSwallowed)
{
    // UAE has a fixed bpnode count. GDB must hear "Cannot insert breakpoint" rather than
    // believe in a breakpoint that does not exist.
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    EXPECT_EQ(Handle(h, ops, "Z0,1000,2"), "OK");
    EXPECT_EQ(Handle(h, ops, "Z0,2000,2"), "OK");
    EXPECT_EQ(Handle(h, ops, "Z0,3000,2"), "OK");
    EXPECT_EQ(Handle(h, ops, "Z0,4000,2"), "E22");
}

TEST(GdbPacketHandlerTest, WatchpointKindsMapToReadWriteAccess)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();

    EXPECT_EQ(Handle(h, ops, "Z2,c216c8,4"), "OK"); // write
    ASSERT_EQ(t.watchpoints.size(), 1u);
    EXPECT_EQ(t.watchpoints[0].addr, 0x00C216C8u);
    EXPECT_EQ(t.watchpoints[0].len, 4u);
    EXPECT_TRUE(t.watchpoints[0].write);
    EXPECT_FALSE(t.watchpoints[0].read);
    EXPECT_EQ(t.watchpoints[0].source, "CPU");

    EXPECT_EQ(Handle(h, ops, "Z3,c216d0,2"), "OK"); // read
    EXPECT_TRUE(t.watchpoints[1].read);
    EXPECT_FALSE(t.watchpoints[1].write);

    // Out of slots.
    EXPECT_EQ(Handle(h, ops, "Z4,c216e0,2"), "E22");
}

TEST(GdbPacketHandlerTest, WatchpointRemovalFindsTheSlotItAllocated)
{
    // GDB names a watchpoint by address; UAE names it by slot. The handler has to remember which
    // slot an address got, or removal silently frees the wrong one.
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    ASSERT_EQ(Handle(h, ops, "Z2,1000,4"), "OK");
    ASSERT_EQ(Handle(h, ops, "Z2,2000,4"), "OK");

    EXPECT_EQ(Handle(h, ops, "z2,2000,4"), "OK");
    ASSERT_EQ(t.removedSlots.size(), 1u);
    EXPECT_EQ(t.removedSlots[0], 1); // the second slot, not the first

    // Removing something already gone is not an error.
    EXPECT_EQ(Handle(h, ops, "z2,2000,4"), "OK");
}

TEST(GdbPacketHandlerTest, ContinueSendsNoImmediateReply)
{
    // The stop reply is asynchronous: it arrives when the machine actually stops. Answering
    // straight away would make GDB think the target had already halted.
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();

    auto r = h.Handle("c", ops);
    EXPECT_FALSE(r.hasReply);
    EXPECT_TRUE(r.resumesTarget);
    EXPECT_EQ(t.resumes, 1);

    r = h.Handle("vCont;c", ops);
    EXPECT_FALSE(r.hasReply);
    EXPECT_TRUE(r.resumesTarget);
    EXPECT_EQ(t.resumes, 2);
}

TEST(GdbPacketHandlerTest, StepRepliesImmediatelyBecauseItIsSynchronous)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();

    auto r = h.Handle("s", ops);
    EXPECT_TRUE(r.hasReply);
    EXPECT_EQ(r.payload, "T05thread:1;");
    EXPECT_EQ(t.steps, 1);

    EXPECT_EQ(Handle(h, ops, "vCont;s"), "T05thread:1;");
    EXPECT_EQ(t.steps, 2);
}

TEST(GdbPacketHandlerTest, VContAdvertisesRangeStepping)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    const std::string caps = Handle(h, ops, "vCont?");
    EXPECT_NE(caps.find(";r"), std::string::npos) << caps;
    EXPECT_NE(caps.find(";c"), std::string::npos);
    EXPECT_NE(caps.find(";s"), std::string::npos);
}

TEST(GdbPacketHandlerTest, RangeSteppingPassesTheRangeAndFallsBackWhenUnavailable)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();

    EXPECT_EQ(Handle(h, ops, "vCont;rc0cb68,c0cb80"), "T05thread:1;");
    ASSERT_EQ(t.rangeSteps.size(), 1u);
    EXPECT_EQ(t.rangeSteps[0].first, 0x00C0CB68u);
    EXPECT_EQ(t.rangeSteps[0].second, 0x00C0CB80u);
    EXPECT_EQ(t.steps, 0); // handled by the range hook, not by single-stepping

    // If the target cannot range-step, one instruction is still a correct answer.
    t.rangeStepSucceeds = false;
    EXPECT_EQ(Handle(h, ops, "vCont;r1000,2000"), "T05thread:1;");
    EXPECT_EQ(t.steps, 1);

    // Malformed range.
    EXPECT_EQ(Handle(h, ops, "vCont;rzzz"), "E16");
}

TEST(GdbPacketHandlerTest, VContStopHaltsTheTarget)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    EXPECT_EQ(Handle(h, ops, "vCont;t"), "T05thread:1;");
    EXPECT_EQ(t.interrupts, 1);
}

TEST(GdbPacketHandlerTest, StopRepliesCarryTheRightReason)
{
    using SR = GdbPacketHandler::StopReason;
    EXPECT_EQ(GdbPacketHandler::FormatStopReply(SR::Trap), "T05thread:1;");
    EXPECT_EQ(GdbPacketHandler::FormatStopReply(SR::Interrupt), "T02thread:1;");
    // swbreak matters: it tells GDB the PC is already at the breakpoint and must not be
    // rewound, which is right for UAE bpnodes (they stop before the instruction runs).
    EXPECT_EQ(GdbPacketHandler::FormatStopReply(SR::Breakpoint), "T05thread:1;swbreak:;");
    EXPECT_EQ(GdbPacketHandler::FormatStopReply(SR::WriteWatch, 0xC216C8),
              "T05thread:1;watch:c216c8;");
    EXPECT_EQ(GdbPacketHandler::FormatStopReply(SR::ReadWatch, 0x1234), "T05thread:1;rwatch:1234;");
    EXPECT_EQ(GdbPacketHandler::FormatStopReply(SR::AccessWatch, 0x1234),
              "T05thread:1;awatch:1234;");
}

TEST(GdbPacketHandlerTest, RunControlWithoutHooksErrorsRatherThanPretending)
{
    GdbTargetOps empty;
    GdbPacketHandler h;
    EXPECT_EQ(h.Handle("c", empty).payload, "E01");
    EXPECT_EQ(h.Handle("s", empty).payload, "E01");
    // No breakpoint hook at all is "unsupported", which is different from "it failed".
    EXPECT_TRUE(h.Handle("Z0,1000,2", empty).payload.empty());
    EXPECT_TRUE(h.Handle("Z2,1000,4", empty).payload.empty());
}

TEST(GdbPacketHandlerTest, MalformedBreakpointPacketsAreRejected)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();
    EXPECT_EQ(Handle(h, ops, "Z0"), "E16");
    EXPECT_EQ(Handle(h, ops, "Z0,"), "E16");
    EXPECT_EQ(Handle(h, ops, "Z0zzz,2"), "E16");
    // An unknown breakpoint type is unsupported rather than an error.
    EXPECT_TRUE(Handle(h, ops, "Z9,1000,2").empty());
}

// --- reverse execution (stage 6) -------------------------------------------------------------

TEST(GdbPacketHandlerTest, ReverseExecutionIsOnlyAdvertisedWhenAvailable)
{
    // Claiming ReverseStep+ without history would put reverse buttons in VS Code that always
    // fail, which is worse than not offering them.
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();

    EXPECT_EQ(Handle(h, ops, "qSupported:swbreak+").find("ReverseStep+"), std::string::npos);

    ops.canReverse = [] { return true; };
    const std::string caps = Handle(h, ops, "qSupported:swbreak+");
    EXPECT_NE(caps.find("ReverseStep+"), std::string::npos);
    EXPECT_NE(caps.find("ReverseContinue+"), std::string::npos);
}

TEST(GdbPacketHandlerTest, ReverseStepAndContinueReportStopsAndExhaustion)
{
    FakeTarget t;
    GdbPacketHandler h;
    auto ops = t.Ops();

    int backSteps = 0;
    bool historyLeft = true;
    ops.reverseStep = [&](std::string& err) {
        if (!historyLeft) {
            err = "no more reverse history";
            return false;
        }
        ++backSteps;
        return true;
    };
    ops.reverseContinue = [&](std::string& err) {
        if (!historyLeft) {
            err = "no more reverse history";
            return false;
        }
        return true;
    };

    EXPECT_EQ(Handle(h, ops, "bs"), "T05thread:1;");
    EXPECT_EQ(backSteps, 1);
    EXPECT_EQ(Handle(h, ops, "bc"), "T05thread:1;");

    // Running out of history is "E." with no code -- GDB renders that as "Reached end of
    // recorded history" rather than an error, which is the honest thing to show.
    historyLeft = false;
    EXPECT_EQ(Handle(h, ops, "bs"), "E.");
    EXPECT_EQ(Handle(h, ops, "bc"), "E.");
}

TEST(GdbPacketHandlerTest, ReversePacketsAreUnsupportedWithoutHooks)
{
    GdbTargetOps empty;
    GdbPacketHandler h;
    EXPECT_TRUE(h.Handle("bs", empty).payload.empty());
    EXPECT_TRUE(h.Handle("bc", empty).payload.empty());
    // An unknown b-packet must not be mistaken for one of them.
    EXPECT_TRUE(h.Handle("bzzz", empty).payload.empty());
}
