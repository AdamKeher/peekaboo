#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace adm::core {
class MemoryAnnotations;
}

namespace adm::ui {

class BreakpointsView;

// Where an address hyperlink lives. Governs whether "Memory dump" / "Disassemble" print into the
// console (Console) or open the dedicated windows (Window).
enum class AddressLinkHost {
    Window,
    Console,
};

// The action a left-click on an address hyperlink performs. Each call site nominates its own;
// Memory (jump to the Memory window) is the default.
enum class AddressDefault {
    Memory,
    Disasm,
    MemoryMap,
    FollowPointer,
};

// Everything the shared address hyperlink + right-click menu needs to act on an address. Built once
// by DebuggerView and shared (by pointer) with every view that draws an address link.
struct AddressActionContext
{
    BreakpointsView* breakpoints = nullptr;         // breakpoint + watchpoint backend
    core::MemoryAnnotations* annotations = nullptr; // label / comment store

    // Window navigation (used when the link is NOT in the console).
    std::function<void(uint32_t addr, size_t len)> jumpToMemory; // Memory window
    std::function<void(uint32_t addr)> jumpToDisasm;             // Disassembly window
    std::function<void(uint32_t addr)> showInMemoryMap;          // Memory Map window

    // Console command runner (memory dump / disassemble from the console host, plus run-to and
    // set-pc from any host). Echoes + runs the command exactly as if typed.
    std::function<void(const std::string& cmd)> runConsoleCommand;

    // Reads the 32-bit long at addr for "Follow pointer". Returns false if unmapped.
    std::function<bool(uint32_t addr, uint32_t& out)> readLong;

    // True while the emulator is paused — gates "Set PC to here".
    std::function<bool()> isPaused;

    bool valid() const { return breakpoints != nullptr; }
};

} // namespace adm::ui
