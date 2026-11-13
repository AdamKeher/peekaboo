#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace adm::core {

/// Control-flow class of a traced instruction (drives call-stack push/pop).
enum class TraceCf : uint8_t {
    Seq,    // ordinary sequential instruction
    Branch, // Bcc / BRA / DBcc — intra-function, no depth change
    Jump,   // JMP — tail dispatch, no depth change (may change function)
    Call,   // BSR / JSR — pushes a frame
    Return  // RTS / RTE / RTR / RTD — pops a frame
};

/// One traced instruction reduced to what the reconstruction needs.
struct TraceStep
{
    uint32_t pc = 0;        // instruction address (runtime)
    uint32_t startLRef = 0; // frame-ref where it executed
    uint32_t endLRef = 0;   // next instruction's lRef (its slot end)
    uint32_t sp = 0;        // stack pointer (A7) at execution — for exception detection
    uint8_t len = 2;        // instruction length in bytes
    TraceCf cf = TraceCf::Seq;
};

/// A reconstructed call-stack frame — one call instance over a time span.
struct CallFrame
{
    uint32_t startLRef = 0;
    uint32_t endLRef = 0;
    int depth = 0;            // 0 = outermost
    uint32_t funcAddress = 0; // link-time function start (0 = unresolved)
    std::string name;         // function name, or "" when unresolved (ROM/library)
};

/**
 * @brief Reconstructs a call-stack flame chart from a CPU trace.
 *
 * Stage 6 of the profiler: turns the flat per-function bands into a hierarchy.
 * Walking the trace in order, a call (BSR/JSR) pushes a frame and a return
 * (RTS/RTE/…) pops one, so callees stack below their caller for exactly the span
 * they run — a time-ordered flame graph. Hardware interrupts/exceptions (a PC
 * divergence not caused by a control instruction) also push, and their RTE pops,
 * so frequent copper/audio interrupts nest correctly instead of corrupting depth.
 *
 * Decoupled from the emulator and from symbol storage: the caller supplies the
 * reduced TraceSteps and a function resolver, so the reconstruction logic is
 * unit-testable with synthetic sequences.
 */
class CallStackProfiler
{
public:
    struct ResolvedFn
    {
        uint32_t address = 0;
        std::string name;
        bool resolved = false;
    };
    using ResolveFn = std::function<ResolvedFn(uint32_t pc)>;

    /// Rebuild frames from the trace. `resolve` maps a runtime PC to its function.
    void Update(const std::vector<TraceStep>& steps, const ResolveFn& resolve);
    void Clear();

    /// All frames, in creation order (a stable order for rendering).
    [[nodiscard]] const std::vector<CallFrame>& Frames() const { return m_frames; }
    [[nodiscard]] int MaxDepth() const { return m_maxDepth; }
    [[nodiscard]] uint32_t GetVersion() const { return m_version; }

private:
    // Guard against pathological traces (runaway recursion / corrupt returns).
    static constexpr int kMaxDepth = 256;

    std::vector<CallFrame> m_frames;
    int m_maxDepth = 0;
    uint32_t m_version = 0;
};

} // namespace adm::core
