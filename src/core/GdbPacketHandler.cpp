#include "GdbPacketHandler.h"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace adm::core {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int HexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Parse a hex number, stopping at the first non-hex character. Returns false if there was no
// digit at all or the value overflows 32 bits.
bool ParseHex32(std::string_view s, uint32_t& out, size_t& consumed)
{
    uint64_t v = 0;
    size_t i = 0;
    for (; i < s.size(); ++i) {
        const int d = HexValue(s[i]);
        if (d < 0)
            break;
        v = (v << 4) | static_cast<uint64_t>(d);
        if (v > 0xFFFFFFFFull)
            return false;
    }
    if (i == 0)
        return false;
    out = static_cast<uint32_t>(v);
    consumed = i;
    return true;
}

// Parse "<hex>,<hex>" as used by m/M/X.
bool ParseAddrLen(std::string_view args, uint32_t& addr, uint32_t& len, size_t& afterLen)
{
    size_t used = 0;
    if (!ParseHex32(args, addr, used))
        return false;
    if (used >= args.size() || args[used] != ',')
        return false;
    std::string_view rest = args.substr(used + 1);
    size_t used2 = 0;
    if (!ParseHex32(rest, len, used2))
        return false;
    afterLen = used + 1 + used2;
    return true;
}

void AppendHex32(std::string& out, uint32_t v)
{
    for (int shift = 28; shift >= 0; shift -= 4)
        out.push_back(kHexDigits[(v >> shift) & 0xF]);
}

bool StartsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

GdbPacketHandler::Response Reply(std::string payload)
{
    GdbPacketHandler::Response r;
    r.payload = std::move(payload);
    return r;
}

// An empty RSP reply is the protocol's "I do not implement this packet". It is a real answer,
// not silence, and GDB relies on receiving it promptly.
GdbPacketHandler::Response Unsupported() { return Reply(std::string()); }

GdbPacketHandler::Response Error(int code)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "E%02X", code & 0xFF);
    return Reply(buf);
}

} // namespace

void GdbPacketHandler::Reset()
{
    m_noAckMode = false;
    m_watchSlots.clear();
}

std::string GdbPacketHandler::FormatStopReply(StopReason reason, uint32_t addr)
{
    char buf[64];
    switch (reason) {
        case StopReason::Interrupt:
            return "T02thread:1;"; // SIGINT
        case StopReason::Breakpoint:
            // "swbreak" tells GDB the PC is already AT the breakpoint address and must not be
            // adjusted backwards -- UAE bpnodes stop before the instruction executes, unlike a
            // classic trap-instruction breakpoint which stops after it.
            return "T05thread:1;swbreak:;";
        case StopReason::WriteWatch:
            std::snprintf(buf, sizeof(buf), "T05thread:1;watch:%x;", addr);
            return buf;
        case StopReason::ReadWatch:
            std::snprintf(buf, sizeof(buf), "T05thread:1;rwatch:%x;", addr);
            return buf;
        case StopReason::AccessWatch:
            std::snprintf(buf, sizeof(buf), "T05thread:1;awatch:%x;", addr);
            return buf;
        case StopReason::Trap:
        default:
            return "T05thread:1;";
    }
}

GdbPacketHandler::Response GdbPacketHandler::Resume(const GdbTargetOps& ops)
{
    if (!ops.cont)
        return Error(0x01);
    ops.cont();
    // Deliberately no reply: GDB now waits for an asynchronous stop reply, which arrives when
    // the machine actually stops (GdbServer::NotifyStopped, from the debug-break callback).
    Response r = Response::None();
    r.resumesTarget = true;
    return r;
}

GdbPacketHandler::Response GdbPacketHandler::StepOne(const GdbTargetOps& ops)
{
    if (!ops.stepOne)
        return Error(0x01);
    ops.stepOne();
    // A single step is synchronous: by the time this returns the machine is stopped again, so
    // the stop reply goes back immediately rather than through the notification path.
    return Reply(FormatStopReply(StopReason::Trap));
}

GdbPacketHandler::Response GdbPacketHandler::HandleBreakpoint(std::string_view packet,
                                                              const GdbTargetOps& ops, bool insert)
{
    // Z<type>,<addr>,<kind>  /  z<type>,<addr>,<kind>
    if (packet.size() < 3 || packet[2] != ',')
        return Error(0x16);
    const char typeChar = packet[1];

    std::string_view rest = packet.substr(3);
    uint32_t addr = 0;
    size_t used = 0;
    if (!ParseHex32(rest, addr, used))
        return Error(0x16);
    uint32_t kind = 2; // for a watchpoint this is the length in bytes
    if (used < rest.size() && rest[used] == ',') {
        size_t used2 = 0;
        ParseHex32(rest.substr(used + 1), kind, used2);
    }

    // Type 0 is a software breakpoint, type 1 a hardware one. Both are implemented the same way
    // here -- a UAE bpnode watching the PC -- because no instruction patching is involved either
    // way, so the distinction carries no meaning for this target.
    if (typeChar == '0' || typeChar == '1') {
        if (insert) {
            if (!ops.addBreakpoint)
                return Unsupported();
            // Report failure honestly: UAE has a fixed bpnode count, and GDB saying "Cannot
            // insert breakpoint" is far better than it believing in one that does not exist.
            return ops.addBreakpoint(addr) ? Reply("OK") : Error(0x22);
        }
        if (!ops.removeBreakpoint)
            return Unsupported();
        return ops.removeBreakpoint(addr) ? Reply("OK") : Error(0x22);
    }

    // 2 = write watch, 3 = read watch, 4 = access watch.
    if (typeChar == '2' || typeChar == '3' || typeChar == '4') {
        if (!ops.addWatchpoint || !ops.removeWatchpoint)
            return Unsupported();
        if (insert) {
            WatchpointSpec spec;
            spec.addr = addr;
            spec.len = kind ? kind : 2;
            spec.read = (typeChar == '3' || typeChar == '4');
            spec.write = (typeChar == '2' || typeChar == '4');
            spec.source = "CPU"; // only CPU accesses have a meaningful break PC
            std::string err;
            const int slot = ops.addWatchpoint(spec, err);
            if (slot < 0)
                return Error(0x22);
            m_watchSlots.emplace_back(addr, slot);
            return Reply("OK");
        }
        for (auto it = m_watchSlots.begin(); it != m_watchSlots.end(); ++it) {
            if (it->first == addr) {
                const bool ok = ops.removeWatchpoint(it->second);
                m_watchSlots.erase(it);
                return ok ? Reply("OK") : Error(0x22);
            }
        }
        return Reply("OK"); // already gone; removing twice is not an error
    }
    return Unsupported();
}

GdbPacketHandler::Response GdbPacketHandler::HandleVCont(std::string_view packet,
                                                         const GdbTargetOps& ops)
{
    if (packet == "vCont?")
        return Reply("vCont;c;C;s;S;t;r");
    if (packet.size() < 6 || packet.substr(0, 6) != "vCont;")
        return Unsupported();

    // Only one thread exists, so the first action wins and any thread-id suffix is ignored.
    std::string_view action = packet.substr(6);
    if (action.empty())
        return Error(0x16);

    switch (action[0]) {
        case 'c':
        case 'C':
            return Resume(ops);
        case 's':
        case 'S':
            return StepOne(ops);
        case 't':
            if (ops.interrupt)
                ops.interrupt();
            return Reply(FormatStopReply(StopReason::Trap));
        case 'r': {
            // vCont;r<start>,<end>[:thread] -- step while the PC stays inside [start, end).
            uint32_t lo = 0, hi = 0;
            size_t used = 0;
            std::string_view args = action.substr(1);
            // GDB emits ";r<start>,<end>" with no separator, but tolerate a space so a
            // hand-typed packet or a future variant does not silently fail to parse.
            while (!args.empty() && args.front() == ' ')
                args.remove_prefix(1);
            if (!ParseHex32(args, lo, used) || used >= args.size() || args[used] != ',')
                return Error(0x16);
            size_t used2 = 0;
            if (!ParseHex32(args.substr(used + 1), hi, used2))
                return Error(0x16);
            if (ops.stepRange && ops.stepRange(lo, hi))
                return Reply(FormatStopReply(StopReason::Trap));
            // Falling back to one instruction is always correct, just slower: GDB asks again.
            return StepOne(ops);
        }
        default:
            return Unsupported();
    }
}

std::string GdbPacketHandler::ToHex(const uint8_t* data, size_t len)
{
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHexDigits[(data[i] >> 4) & 0xF]);
        out.push_back(kHexDigits[data[i] & 0xF]);
    }
    return out;
}

bool GdbPacketHandler::FromHex(std::string_view hex, std::vector<uint8_t>& out)
{
    if (hex.size() % 2 != 0)
        return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = HexValue(hex[i]);
        const int lo = HexValue(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

bool GdbPacketHandler::DecodeBinary(std::string_view raw, std::vector<uint8_t>& out)
{
    out.clear();
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '}') {
            // Escaped byte: the next character XOR 0x20.
            if (i + 1 >= raw.size())
                return false;
            out.push_back(static_cast<uint8_t>(raw[++i]) ^ 0x20);
            continue;
        }
        if (c == '*') {
            // Run-length: repeat the previous byte (count + 29) more times.
            if (out.empty() || i + 1 >= raw.size())
                return false;
            const int repeat = static_cast<unsigned char>(raw[++i]) - 29;
            if (repeat < 0)
                return false;
            out.insert(out.end(), static_cast<size_t>(repeat), out.back());
            continue;
        }
        out.push_back(static_cast<uint8_t>(c));
    }
    return true;
}

std::string GdbPacketHandler::RunLengthEncode(std::string_view payload)
{
    std::string out;
    out.reserve(payload.size());
    size_t i = 0;
    while (i < payload.size()) {
        const char c = payload[i];
        size_t run = 1;
        while (i + run < payload.size() && payload[i + run] == c)
            ++run;

        out.push_back(c);
        // The first character is emitted literally, so only run-1 repeats remain. Encoding is
        // only a win from four repeats up, and '#'/'$' must never appear as a count byte.
        size_t remaining = run - 1;
        while (remaining >= 4) {
            size_t chunk = std::min<size_t>(remaining, 97); // 29+97 = 126 ('~'), the top of range
            int countChar = static_cast<int>(chunk) + 29;
            if (countChar == '#' || countChar == '$') {
                --chunk;
                --countChar;
            }
            out.push_back('*');
            out.push_back(static_cast<char>(countChar));
            remaining -= chunk;
        }
        out.append(remaining, c);
        i += run;
    }
    // Never return an encoding that is bigger than what we started with.
    return out.size() < payload.size() ? out : std::string(payload);
}

std::string GdbPacketHandler::FormatGPacket(const GdbRegs& regs)
{
    std::string out;
    out.reserve(kGPacketBytes * 2);
    for (int i = 0; i < 8; ++i)
        AppendHex32(out, regs.d[i]);
    for (int i = 0; i < 7; ++i) // a0-a6
        AppendHex32(out, regs.a[i]);
    AppendHex32(out, regs.a[7]); // sp
    AppendHex32(out, regs.sr);   // 16-bit register, 32-bit slot
    AppendHex32(out, regs.pc);
    // fp0-fp7, fpcontrol, fpstatus, fpiaddr: not modelled. 'x' marks a register as unavailable,
    // which GDB renders as "<unavailable>" instead of a fabricated value.
    out.append((kGPacketBytes - kIntRegBytes) * 2, 'x');
    return out;
}

GdbPacketHandler::Response GdbPacketHandler::HandleReadMemory(std::string_view args,
                                                              const GdbTargetOps& ops)
{
    uint32_t addr = 0, len = 0;
    size_t consumed = 0;
    if (!ParseAddrLen(args, addr, len, consumed))
        return Error(0x16); // EINVAL
    if (len == 0)
        return Reply(std::string());
    if (!ops.readMem)
        return Error(0x01);
    // Never let a client's length claim allocate unboundedly.
    len = std::min<uint32_t>(len, static_cast<uint32_t>(kMaxPacketSize / 2));

    std::vector<uint8_t> buf(len);
    std::vector<bool> valid;
    ops.readMem(addr, buf.data(), len, &valid);

    // RSP has no way to say "these bytes are fine and those are not" inside one reply, so any
    // unmapped byte makes the whole read an error. Reporting E01 is what lets GDB print
    // "Cannot access memory at address 0x..." rather than silently trusting invented zeroes.
    for (uint32_t i = 0; i < len; ++i) {
        const bool ok = (i >= valid.size()) || valid[i];
        if (!ok)
            return Error(0x01);
    }
    return Reply(RunLengthEncode(ToHex(buf.data(), buf.size())));
}

GdbPacketHandler::Response GdbPacketHandler::HandleWriteMemory(std::string_view args,
                                                               const GdbTargetOps& ops, bool binary)
{
    uint32_t addr = 0, len = 0;
    size_t afterLen = 0;
    if (!ParseAddrLen(args, addr, len, afterLen))
        return Error(0x16);
    if (afterLen >= args.size() || args[afterLen] != ':')
        return Error(0x16);
    std::string_view data = args.substr(afterLen + 1);

    std::vector<uint8_t> bytes;
    if (binary) {
        if (!DecodeBinary(data, bytes))
            return Error(0x16);
    } else if (!FromHex(data, bytes)) {
        return Error(0x16);
    }
    // A zero-length X packet is GDB probing whether binary writes are supported at all.
    if (len == 0 && bytes.empty())
        return Reply("OK");
    if (bytes.size() != len)
        return Error(0x16);
    if (!ops.writeMem)
        return Error(0x01);

    std::string err;
    if (!ops.writeMem(addr, bytes.data(), bytes.size(), err))
        return Error(0x01);
    return Reply("OK");
}

GdbPacketHandler::Response GdbPacketHandler::HandleReadRegister(std::string_view args,
                                                                const GdbTargetOps& ops)
{
    uint32_t regnum = 0;
    size_t used = 0;
    if (!ParseHex32(args, regnum, used))
        return Error(0x16);
    if (regnum >= static_cast<uint32_t>(kRegCount))
        return Error(0x16);
    // FPU registers: report unavailable rather than zero. fp0-fp7 are 12 bytes, the three
    // control registers 4.
    if (regnum >= static_cast<uint32_t>(kIntRegCount)) {
        const size_t bytes = (regnum <= 25) ? 12 : 4;
        return Reply(std::string(bytes * 2, 'x'));
    }
    if (!ops.readRegs)
        return Error(0x01);
    GdbRegs regs;
    if (!ops.readRegs(regs))
        return Error(0x01);
    const std::string g = FormatGPacket(regs);
    return Reply(g.substr(regnum * 8, 8));
}

GdbPacketHandler::Response GdbPacketHandler::HandleWriteRegister(std::string_view args,
                                                                 const GdbTargetOps& ops)
{
    uint32_t regnum = 0;
    size_t used = 0;
    if (!ParseHex32(args, regnum, used))
        return Error(0x16);
    if (used >= args.size() || args[used] != '=')
        return Error(0x16);
    std::vector<uint8_t> bytes;
    if (!FromHex(args.substr(used + 1), bytes) || bytes.size() != 4)
        return Error(0x16);
    if (regnum >= static_cast<uint32_t>(kIntRegCount))
        return Error(0x01); // FPU state is not modelled, so refuse rather than pretend
    if (!ops.writeReg)
        return Error(0x01);

    const uint32_t value = (static_cast<uint32_t>(bytes[0]) << 24) |
                           (static_cast<uint32_t>(bytes[1]) << 16) |
                           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
    if (!ops.writeReg(static_cast<int>(regnum), value))
        return Error(0x01);
    return Reply("OK");
}

GdbPacketHandler::Response GdbPacketHandler::HandleWriteRegisters(std::string_view args,
                                                                  const GdbTargetOps& ops)
{
    // Accept a short packet (integer registers only) as well as a full 180-byte one: GDB sends
    // back exactly what it was given, and we hand it 'xx' for the FPU half.
    if (args.size() < kIntRegBytes * 2)
        return Error(0x16);
    if (!ops.writeReg)
        return Error(0x01);
    for (int i = 0; i < kIntRegCount; ++i) {
        std::string_view field = args.substr(static_cast<size_t>(i) * 8, 8);
        // A register the client marked unavailable is left alone rather than zeroed.
        if (field.find('x') != std::string_view::npos)
            continue;
        uint32_t value = 0;
        size_t used = 0;
        if (!ParseHex32(field, value, used) || used != 8)
            return Error(0x16);
        if (!ops.writeReg(i, value))
            return Error(0x01);
    }
    return Reply("OK");
}

GdbPacketHandler::Response GdbPacketHandler::HandleQuery(std::string_view packet,
                                                         const GdbTargetOps& ops)
{
    if (StartsWith(packet, "qSupported")) {
        std::string caps = "PacketSize=";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%zx", kMaxPacketSize);
        caps += buf;
        // No qXfer:features:read - we deliberately serve GDB's built-in m68k layout instead of
        // a target description, which is what removes the expat dependency.
        caps += ";QStartNoAckMode+;swbreak+;vContSupported+;qXfer:features:read-";
        // Only claim reverse execution when the target can actually do it: advertising it
        // without frame history would put live-looking buttons in the UI that always fail.
        if (ops.canReverse && ops.canReverse())
            caps += ";ReverseStep+;ReverseContinue+";
        return Reply(caps);
    }
    if (packet == "qAttached")
        return Reply("1"); // we attached to an already-running program; kill must not reboot it
    if (packet == "qC")
        return Reply("QC1");
    if (packet == "qfThreadInfo")
        return Reply("m1"); // a single, synthetic thread: the 68000 has one context
    if (packet == "qsThreadInfo")
        return Reply("l");
    if (StartsWith(packet, "qSymbol"))
        return Reply("OK"); // we resolve symbols host-side; no lookups needed from GDB
    if (StartsWith(packet, "qRcmd,")) {
        if (!ops.monitor)
            return Unsupported();
        std::vector<uint8_t> cmd;
        if (!FromHex(packet.substr(6), cmd))
            return Error(0x16);
        const std::string text = ops.monitor(std::string(cmd.begin(), cmd.end()));
        if (text.empty())
            return Reply("OK");
        return Reply(ToHex(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    }
    // qTStatus, qOffsets, qThreadExtraInfo and friends: answering "unsupported" is correct and
    // keeps GDB from waiting.
    return Unsupported();
}

GdbPacketHandler::Response GdbPacketHandler::Handle(std::string_view packet,
                                                    const GdbTargetOps& ops)
{
    if (packet.empty())
        return Unsupported();

    switch (packet[0]) {
        case 'q':
            return HandleQuery(packet, ops);

        case 'Q':
            if (packet == "QStartNoAckMode") {
                // Order matters: the OK for this packet is still acknowledged normally; no-ack
                // applies from the next one. The caller flushes this reply before consulting
                // NoAckMode(), so setting it here is safe.
                m_noAckMode = true;
                return Reply("OK");
            }
            return Unsupported();

        case '?':
            // RSP's contract is that the target is stopped when gdb attaches, and gdb relies on
            // it: believing the target is already stopped, it never resumes, and because Z0
            // packets are only sent on resume the breakpoints never reach the target at all.
            // Claiming T05 while the machine ran free is what made a breakpoint set in VS Code
            // appear in the gutter, never appear in Peekaboo's list, and never fire.
            // So make the claim true rather than merely plausible.
            if (ops.isHalted && !ops.isHalted() && ops.interrupt)
                ops.interrupt();
            return Reply("T05thread:1;");

        case 'g': {
            if (!ops.readRegs)
                return Error(0x01);
            GdbRegs regs;
            if (!ops.readRegs(regs))
                return Error(0x01);
            return Reply(FormatGPacket(regs));
        }

        case 'G':
            return HandleWriteRegisters(packet.substr(1), ops);

        case 'p':
            return HandleReadRegister(packet.substr(1), ops);

        case 'P':
            return HandleWriteRegister(packet.substr(1), ops);

        case 'm':
            return HandleReadMemory(packet.substr(1), ops);

        case 'M':
            return HandleWriteMemory(packet.substr(1), ops, /*binary=*/false);

        case 'X':
            return HandleWriteMemory(packet.substr(1), ops, /*binary=*/true);

        case 'H':
            // Thread selection. There is only one context, so any selection succeeds.
            return Reply("OK");

        case 'D':
        case 'k': {
            Response r = Reply("OK");
            r.closeConnection = true;
            return r;
        }

        case 'v':
            if (StartsWith(packet, "vKill")) {
                Response r = Reply("OK");
                r.closeConnection = true;
                return r;
            }
            if (StartsWith(packet, "vCont"))
                return HandleVCont(packet, ops);
            return Unsupported();

        case 'c':
        case 'C':
            return Resume(ops);

        case 's':
        case 'S':
            return StepOne(ops);

        case 'b':
            // bs / bc -- step or continue backwards. GDB only sends these after we advertise
            // ReverseStep+/ReverseContinue+ in qSupported.
            if (packet == "bs" || packet == "bc") {
                const auto& fn = (packet == "bs") ? ops.reverseStep : ops.reverseContinue;
                if (!fn)
                    return Unsupported();
                std::string err;
                if (fn(err))
                    return Reply(FormatStopReply(StopReason::Trap));
                // "E." with no code is how RSP says the target cannot go further back; GDB
                // turns it into "Reached end of recorded history" rather than an error.
                return Reply("E.");
            }
            return Unsupported();

        case 'Z':
            return HandleBreakpoint(packet, ops, /*insert=*/true);

        case 'z':
            return HandleBreakpoint(packet, ops, /*insert=*/false);

        case '!':
            return Reply("OK"); // extended-remote mode acknowledged

        default:
            return Unsupported();
    }
}

} // namespace adm::core
