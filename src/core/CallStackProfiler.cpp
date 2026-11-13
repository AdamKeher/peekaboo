#include "CallStackProfiler.h"

#include <algorithm>

namespace adm::core {

void CallStackProfiler::Clear()
{
    m_frames.clear();
    m_maxDepth = 0;
    ++m_version;
}

void CallStackProfiler::Update(const std::vector<TraceStep>& steps, const ResolveFn& resolve)
{
    m_frames.clear();
    m_maxDepth = 0;
    ++m_version;
    if (steps.empty()) {
        return;
    }

    // Open frames, indexed by depth. Each holds its start and resolved function;
    // it is committed to m_frames (with an end) when it is popped or the trace ends.
    struct Open
    {
        uint32_t startLRef;
        ResolvedFn fn;
    };
    std::vector<Open> stack;

    auto openFrame = [&](uint32_t startLRef, const ResolvedFn& fn) {
        if (static_cast<int>(stack.size()) < kMaxDepth) {
            stack.push_back({startLRef, fn});
        }
    };
    auto commit = [&](const Open& o, uint32_t endLRef, int depth) {
        CallFrame f;
        f.startLRef = o.startLRef;
        f.endLRef = endLRef;
        f.depth = depth;
        f.funcAddress = o.fn.address;
        f.name = o.fn.name;
        m_frames.push_back(std::move(f));
        m_maxDepth = std::max(m_maxDepth, depth);
    };

    // Seed with the outermost frame for the first instruction.
    openFrame(steps[0].startLRef, resolve(steps[0].pc));

    for (size_t i = 1; i < steps.size(); ++i) {
        const TraceStep& prev = steps[i - 1];
        const TraceStep& cur = steps[i];
        ResolvedFn curFn = resolve(cur.pc);

        const bool prevCall = (prev.cf == TraceCf::Call);
        const bool prevReturn = (prev.cf == TraceCf::Return);
        // A hardware interrupt / exception entry is a PC divergence after a plain
        // sequential instruction (a branch/jump/call would explain a normal
        // divergence) that ALSO pushes an exception frame — the stack pointer drops
        // by at least the 6-byte 68000 frame (SR + PC). Requiring the SP drop is what
        // rejects spurious divergences (mis-sized instructions, trace gaps) that would
        // otherwise inflate depth without bound.
        const bool spDroppedFrame = (prev.sp >= cur.sp) && (prev.sp - cur.sp >= 6);
        const bool exceptionEntry =
            (prev.cf == TraceCf::Seq) && (cur.pc != prev.pc + prev.len) && spDroppedFrame;

        if (prevReturn) {
            if (!stack.empty()) {
                commit(stack.back(), cur.startLRef, static_cast<int>(stack.size()) - 1);
                stack.pop_back();
            }
            // Underflow (trace began mid-call, or unbalanced RTE): re-seed a root.
            if (stack.empty()) {
                openFrame(cur.startLRef, curFn);
            }
        } else if (prevCall || exceptionEntry) {
            openFrame(cur.startLRef, curFn);
        } else if (!stack.empty() && stack.back().fn.address != curFn.address) {
            // Same depth but the function changed without a call/return: a tail JMP
            // or fall-through into an adjacent function. Close and reopen in place.
            commit(stack.back(), cur.startLRef, static_cast<int>(stack.size()) - 1);
            stack.back() = {cur.startLRef, curFn};
        }
    }

    // Close everything still open at the end of the trace.
    const uint32_t endLRef = steps.back().endLRef;
    for (int d = static_cast<int>(stack.size()) - 1; d >= 0; --d) {
        commit(stack[static_cast<size_t>(d)], endLRef, d);
    }
}

} // namespace adm::core
