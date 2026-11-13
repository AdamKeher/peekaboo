#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "CallStackProfiler.h" // CallFrame
#include "CpuProfiler.h"       // InstructionStats
#include "FunctionProfiler.h"  // FunctionStats
#include "StackUnwinder.h"     // UnwoundFrame

namespace adm::core {

// Which profiling surface a query wants. Mirrors the debugger's live profiling views:
//   Functions    — per-function cycle rollup (Disassembly > Profile > Group by function)
//   Instructions — per-PC hot instructions (Disassembly > Profile)
//   Flat         — flat leaf-function bands over the frame (Frame Scrubber, Flat mode)
//   Flame        — call-stack flame graph over the frame (Frame Scrubber, Flame mode)
//   CallStack    — the call stack at the scrubber playhead (Call Stack window)
enum class ProfileView { Functions, Instructions, Flat, Flame, CallStack };

struct ProfileQuery
{
    ProfileView view = ProfileView::Functions;
    int limit = 32; // max rows returned (<= 0 means "all")
};

// A self-contained snapshot of the last traced frame's profiling data, produced on demand
// by DebuggerView and consumed by the MCP get_profile tool + the AR console PROFILE /
// CALLSTACK commands. Only the vector matching the requested view is populated; the summary
// fields are always filled so callers can show context.
struct ProfileSnapshot
{
    bool available = false;   // any CPU trace was captured this frame
    bool haveSymbols = false; // ELF symbols loaded (functions/flame resolvable by name)
    uint32_t totalCycles = 0;
    uint32_t resolvedCycles = 0;   // cycles attributed to a named function
    uint32_t unresolvedCycles = 0; // ROM / library / inter-function gaps
    uint32_t playheadLRef = 0;     // scrubber position (CallStack view)

    std::vector<InstructionStats> instructions; // ProfileView::Instructions
    std::vector<FunctionStats> functions;       // ProfileView::Functions
    std::vector<CallFrame> frames;              // ProfileView::Flat / Flame
    std::vector<CallFrame> callStack;           // ProfileView::CallStack (outermost first)

    // ProfileView::CallStack also carries the *live* stack: a real DWARF unwind of the
    // halted CPU, which needs no CPU trace and is available at any breakpoint or step.
    // The two answer different questions, so both are reported rather than one replacing
    // the other. Empty and false when the machine is running.
    bool liveAvailable = false;
    std::vector<UnwoundFrame> liveStack; // innermost first
    std::string liveStop;                // why the walk ended, human-readable
};

// Canonical lowercase token for a view (used in JSON / console echo).
inline const char* ProfileViewName(ProfileView v)
{
    switch (v) {
        case ProfileView::Functions:
            return "functions";
        case ProfileView::Instructions:
            return "instructions";
        case ProfileView::Flat:
            return "flat";
        case ProfileView::Flame:
            return "flame";
        case ProfileView::CallStack:
            return "callstack";
    }
    return "functions";
}

// Parse a case-insensitive view name, accepting the common short aliases. Returns false if
// the token is unrecognized (callers report the valid set).
inline bool ParseProfileView(const std::string& s, ProfileView& out)
{
    std::string t;
    t.reserve(s.size());
    for (char c : s)
        t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (t == "functions" || t == "function" || t == "func" || t == "fn") {
        out = ProfileView::Functions;
        return true;
    }
    if (t == "instructions" || t == "instruction" || t == "instr" || t == "ins") {
        out = ProfileView::Instructions;
        return true;
    }
    if (t == "flat") {
        out = ProfileView::Flat;
        return true;
    }
    if (t == "flame") {
        out = ProfileView::Flame;
        return true;
    }
    if (t == "callstack" || t == "call-stack" || t == "stack" || t == "cs") {
        out = ProfileView::CallStack;
        return true;
    }
    return false;
}

} // namespace adm::core
