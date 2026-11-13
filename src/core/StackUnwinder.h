#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace adm::core {

class DebugFrame;
class DebugInline;
class DebugLines;
class DebugSymbols;

/// One frame of a backtrace, innermost first.
struct UnwoundFrame
{
    uint32_t pc = 0;  // runtime address; for callers this is the return address
    uint32_t sp = 0;  // stack pointer on entry to this frame
    uint32_t cfa = 0; // canonical frame address (== the caller's sp)
    std::string name; // empty when no symbol covers the pc
    std::string file; // source path, empty when unknown
    uint32_t line = 0;
    /**
     * Synthesized from DWARF inline info rather than from a real stack frame. On an LTO
     * build most of what a programmer thinks of as the call stack is inlined and has no
     * frame at all, so leaving these out would show `main` calling a leaf directly.
     * Shares its caller's pc/sp/cfa.
     */
    bool inlined = false;
};

/// Why the walk stopped. The distinction is the point: a backtrace that ends because the
/// program ran out of frames and one that ends because we lost the trail look identical
/// unless the reason is carried out with the frames.
enum class UnwindStop {
    EndOfStack,     // the outermost frame had no recoverable return address
    NoCfi,          // the pc left the code covered by .debug_frame
    NoDebugInfo,    // there was no CFI to begin with
    UnmappedMemory, // a stack read fell outside emulated RAM, or gave an odd pc
    DepthLimit,     // hit the frame cap
    NoProgress,     // the stack pointer stopped increasing (corrupt or mid-prologue)
};

struct UnwindResult
{
    std::vector<UnwoundFrame> frames; // innermost first
    UnwindStop stop = UnwindStop::EndOfStack;
    uint32_t stopPc = 0; // the pc we could not unwind past, for the terminal message

    [[nodiscard]] bool Empty() const { return frames.empty(); }
};

/// The m68k register file the unwinder needs, in DWARF numbering order (d0-d7, a0-a7).
struct UnwindRegs
{
    std::array<uint32_t, 16> reg{}; // 0-7 = d0-d7, 8-15 = a0-a7 (15 == sp)
    uint32_t pc = 0;
};

struct UnwindInputs
{
    UnwindRegs regs;

    /// Read a big-endian longword from emulated memory. False when unmapped.
    std::function<bool(uint32_t addr, uint32_t& out)> readLong;

    /// Runtime address -> link-time address, or nullopt when the address is not part of
    /// the loaded program (ROM, a library, another hunk).
    std::function<std::optional<uint32_t>(uint32_t runtimeAddr)> toLink;

    const DebugFrame* frames = nullptr;
    const DebugSymbols* symbols = nullptr;
    const DebugLines* lines = nullptr;
    const DebugInline* inlines = nullptr;

    uint32_t maxDepth = 64;
};

/**
 * @brief Walks a real m68k call stack using DWARF call-frame information.
 *
 * This is the same job gdb does when VS Code asks it for a backtrace, done in-process so
 * that Peekaboo's own breakpoints, stepping and console can show a call stack without a
 * gdb session attached, and without the CPU trace the frame-scrubber reconstruction needs.
 *
 * Why CFI rather than walking a frame-pointer chain: the bartman toolchain compiles with
 * no frame pointer, so a6 is an ordinary callee-saved register and there is no chain to
 * follow. The CFA at any instruction is `a7 + N` for an N that changes at nearly every
 * push and pop, and only .debug_frame records it.
 *
 * Pure: memory and address translation come in as callbacks, so this unit-tests against
 * synthetic memory with no emulator.
 */
class StackUnwinder
{
public:
    static UnwindResult Unwind(const UnwindInputs& in);
};

/// One-line human-readable explanation of a stop, for the UI's terminal row and the
/// console. `region` is the memory region name at stopPc, when one is known.
[[nodiscard]] std::string DescribeUnwindStop(const UnwindResult& result, const std::string& region);

} // namespace adm::core
