#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "GdbTargetOps.h"

namespace adm::core {

/**
 * @brief Pure GDB remote-serial-protocol packet handler.
 *
 * Takes the *payload* of one RSP packet (the bytes between `$` and `#`, framing and checksum
 * already stripped) and produces the payload to send back. It owns no sockets, spawns no
 * threads and touches no global state, so the whole protocol surface is unit-testable against
 * a fake GdbTargetOps.
 *
 * Register layout note: GDB's default m68k target description is 29 registers / 180 bytes --
 * d0-d7, a0-a6, sp, sr, pc, then fp0-fp7 (12 bytes each) and fpcontrol/fpstatus/fpiaddr. We
 * serve the 18 integer registers and emit the 108 FPU bytes as `xx`, which RSP defines as
 * "unavailable". Verified against GNU gdb 17.0.50 (m68k-amiga-elf): it accepts this and prints
 * the integer registers correctly, which is why we need neither a custom target.xml nor an
 * expat-enabled gdb the way Bartman's toolchain does.
 */
class GdbPacketHandler
{
public:
    struct Response
    {
        std::string payload;
        // True when the reply hands control back to the target (c/s/vCont). The pump must stop
        // servicing packets and return to the emulator loop immediately.
        bool resumesTarget = false;
        // True when the client asked to end the session (D/k/vKill) and the socket should close
        // after this reply is flushed.
        bool closeConnection = false;
        // False for packets that must not be answered at all (bare acks). An empty payload is a
        // meaningful RSP reply ("unsupported"), so emptiness alone cannot signal silence.
        bool hasReply = true;

        static Response None()
        {
            Response r;
            r.hasReply = false;
            return r;
        }
    };

    /// Number of registers and total byte width of a `g` packet, per GDB's m68k description.
    static constexpr int kRegCount = 29;
    static constexpr size_t kGPacketBytes = 180;
    /// The 18 integer registers occupy the first 72 bytes; the rest is FPU state we do not serve.
    static constexpr int kIntRegCount = 18;
    static constexpr size_t kIntRegBytes = 72;
    /// Largest payload we advertise in qSupported.
    static constexpr size_t kMaxPacketSize = 0x4000;

    GdbPacketHandler() = default;

    /// Handle one packet payload. `ops` supplies everything that touches the machine.
    Response Handle(std::string_view packet, const GdbTargetOps& ops);

    /// True once the client has successfully negotiated QStartNoAckMode.
    [[nodiscard]] bool NoAckMode() const { return m_noAckMode; }

    /// Forget per-connection negotiated state. Call when a client disconnects.
    void Reset();

    /// Why the target stopped, for the T-packet GDB receives.
    enum class StopReason {
        Trap,        // breakpoint, step, or a plain halt
        Breakpoint,  // a Z0 we placed: adds "swbreak:;" so GDB does not re-step the PC
        WriteWatch,  // Z2
        ReadWatch,   // Z3
        AccessWatch, // Z4
        Interrupt,   // Ctrl-C
    };

    /// Build a stop reply ("T05thread:1;", "T05watch:c216c8;", "T02thread:1;", ...).
    static std::string FormatStopReply(StopReason reason, uint32_t addr = 0);

    // --- helpers, exposed for testing -------------------------------------------------------

    /// Format a GdbRegs as the 360-hex-character `g` reply (FPU registers as `xx`).
    static std::string FormatGPacket(const GdbRegs& regs);
    /// Hex-encode bytes, lowercase, two characters per byte.
    static std::string ToHex(const uint8_t* data, size_t len);
    /// Decode an even-length hex string. Returns false on odd length or a non-hex digit.
    static bool FromHex(std::string_view hex, std::vector<uint8_t>& out);
    /**
     * @brief Decode the `X` packet's binary payload.
     *
     * RSP escapes `}`, `#`, `$` and `*` as `}` followed by the byte XOR 0x20. Run-length
     * encoding (`<byte>*<count+29>`) may also appear. Returns false on a malformed escape.
     */
    static bool DecodeBinary(std::string_view raw, std::vector<uint8_t>& out);
    /**
     * @brief Apply RSP run-length encoding to a reply.
     *
     * Collapses runs into `<byte>*<len+29>`, which matters because reads of large zero-filled
     * regions are common and otherwise cost two characters per byte. `*` never encodes a run
     * shorter than four, and the count byte stays inside the printable range RSP allows.
     */
    static std::string RunLengthEncode(std::string_view payload);

private:
    Response HandleQuery(std::string_view packet, const GdbTargetOps& ops);
    Response HandleReadMemory(std::string_view args, const GdbTargetOps& ops);
    Response HandleWriteMemory(std::string_view args, const GdbTargetOps& ops, bool binary);
    Response HandleReadRegister(std::string_view args, const GdbTargetOps& ops);
    Response HandleWriteRegister(std::string_view args, const GdbTargetOps& ops);
    Response HandleWriteRegisters(std::string_view args, const GdbTargetOps& ops);
    Response HandleBreakpoint(std::string_view packet, const GdbTargetOps& ops, bool insert);
    Response HandleVCont(std::string_view packet, const GdbTargetOps& ops);
    Response Resume(const GdbTargetOps& ops);
    Response StepOne(const GdbTargetOps& ops);

    bool m_noAckMode = false;
    // Watchpoint address -> UAE slot, so a z2/z3/z4 can remove what the matching Z inserted.
    // GDB identifies a watchpoint by address+kind, UAE by slot number.
    std::vector<std::pair<uint32_t, int>> m_watchSlots;
};

} // namespace adm::core
