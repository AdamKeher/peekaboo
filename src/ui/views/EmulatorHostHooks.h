#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../core/AmigaEmulator.h"
#include "../../core/ArMemoryAccess.h"
#include "../../core/HexUtil.h"
#include "BreakpointsView.h"
#include "EmulatorView.h"

namespace adm::ui {

// Points `mem` at the emulator's current memory map and wires the chipset/CIA hooks that
// cannot be reached through a host pointer. Every consumer of ArMemoryAccess needs exactly
// this - the AR console, the GDB server and the memory-lock resolver - so it lives here
// rather than being reimplemented per call site with subtly different hooks.
//
// Reads come from side-effect-free snapshots (reading CIA ICR on the real chip clears it, and
// a debugger read must not); writes go straight to the chip.
inline void BuildEmulatorMemoryAccess(core::ArMemoryAccess& mem, adm::AmigaEmulator* emu)
{
    std::vector<core::ArMemoryAccess::Region> regions;
    if (emu) {
        for (const auto& r : emu->GetMemoryRegions())
            regions.push_back({r.hostPtr, r.amigaAddr, r.size, r.name, r.writable});
    }
    mem.SetRegions(std::move(regions));

    mem.readCustomReg = [emu](uint32_t offset, uint16_t& out) {
        return emu && emu->ReadCustomRegisterWord(offset, out);
    };
    mem.writeCustomReg = [emu](uint32_t offset, uint16_t value) {
        return emu && emu->WriteCustomRegister(core::ArMemoryAccess::kCustomBase + offset, value);
    };
    mem.readCiaReg = [emu](bool chipB, int reg, uint8_t& out) {
        return emu && emu->ReadCIARegisterByte(chipB, reg, out);
    };
    mem.writeCiaReg = [emu](bool chipB, int reg, uint8_t value) {
        if (!emu)
            return false;
        // CIA-A answers on odd addresses ($BFE001 + reg*$100), CIA-B on even ($BFD000 + ...).
        const uint32_t addr = chipB ? (0xBFD000u + static_cast<uint32_t>(reg) * 0x100u)
                                    : (0xBFE001u + static_cast<uint32_t>(reg) * 0x100u);
        return emu->WriteCIARegister(addr, value);
    };
}

// Fills the execution-control and breakpoint/watchpoint hooks that the AR console's
// ArHost and the MCP server's McpHost wire identically. They are unrelated structs
// that happen to share these field names, so this is templated on the host type.
// Host-specific hooks (annotations, documents, console-only commands, etc.) are left
// untouched for the caller to set.
//
// stepInto is assigned a bool-returning lambda: std::function<bool()> takes it
// directly, and std::function<void()> accepts it too (the result is discarded), so
// the same wiring serves both hosts despite their differing stepInto signatures.
template <typename Host>
void WireEmulatorHostHooks(Host& host, EmulatorView* view, BreakpointsView* bps)
{
    auto emulator = [view]() -> adm::AmigaEmulator* {
        return view ? view->GetEmulator() : nullptr;
    };

    host.isRunning = [view]() { return view && view->IsRunning(); };
    host.isPaused = [view]() { return view && view->IsPaused(); };
    host.pause = [view]() {
        if (view && view->IsRunning() && !view->IsPaused())
            view->TogglePause();
    };
    host.resume = [view, emulator]() {
        auto* emu = emulator();
        if (emu && emu->HasArmDebug())
            emu->ArmDebugTrace();
        if (view && view->IsPaused())
            view->TogglePause();
    };
    host.stepInto = [view]() -> bool {
        if (!view)
            return false;
        view->StepInstruction();
        return true;
    };
    host.stepOver = [view]() { return view ? view->StepOver() : false; };
    host.runUntilFrame = [view](uint64_t targetFrame, bool turbo) {
        return view && view->RunUntilFrame(targetFrame, turbo);
    };

    host.addBreakpoint = [bps](uint32_t addr) { return bps && bps->AddBreakpointAt(addr, "PC"); };
    host.addSmartBreakpoint = [bps](uint32_t addr, bool condEnabled, const std::string& reg,
                                    const std::string& oper, uint32_t value, uint32_t mask,
                                    int ignoreCount, bool oneShot) {
        return bps && bps->AddSmartBreakpointAt(addr, condEnabled, reg, oper, value, mask,
                                                ignoreCount, oneShot);
    };
    host.removeBreakpoint = [bps](uint32_t addr) { return bps && bps->RemoveEntryAt(addr, "PC"); };
    host.listBreakpoints = [bps]() {
        std::vector<std::pair<uint32_t, bool>> out;
        if (!bps)
            return out;
        for (const auto& e : bps->GetEntries()) {
            if (e.type != "PC")
                continue;
            if (auto addr = core::ParseHexAddress(e.address))
                out.push_back({*addr, e.enabled});
        }
        return out;
    };
    // Break-on-KPrintF-text. The rules live in AmigaEmulator (which does the matching against the
    // serial debug output), so the console, MCP and the Breakpoints window all drive one store.
    host.addKprintfBreakpoint = [bps](const std::string& text, core::KprintfMatch match,
                                      bool caseSensitive, bool oneShot) -> int {
        auto* kp = bps ? bps->GetKprintfBreakpoints() : nullptr;
        return kp ? kp->Add(text, match, caseSensitive, oneShot) : 0;
    };
    host.removeKprintfBreakpoint = [bps](int id) {
        auto* kp = bps ? bps->GetKprintfBreakpoints() : nullptr;
        return kp ? kp->Remove(id) : false;
    };
    host.clearKprintfBreakpoints = [bps]() {
        if (auto* kp = bps ? bps->GetKprintfBreakpoints() : nullptr)
            kp->Clear();
    };
    host.listKprintfBreakpoints = [bps]() {
        auto* kp = bps ? bps->GetKprintfBreakpoints() : nullptr;
        return kp ? kp->Entries() : std::vector<core::KprintfBreakpoint>{};
    };
    host.addWatchpoint = [bps](uint32_t addr) {
        return bps && bps->AddBreakpointAt(addr, "Write");
    };
    host.removeWatchpoint = [bps](uint32_t addr) {
        return bps && bps->RemoveEntryAt(addr, "Write");
    };
    host.listWatchpoints = [bps]() {
        std::vector<uint32_t> out;
        if (!bps)
            return out;
        for (const auto& e : bps->GetEntries()) {
            if (e.type == "PC")
                continue;
            if (auto addr = core::ParseHexAddress(e.address))
                out.push_back(*addr);
        }
        return out;
    };
}

} // namespace adm::ui
