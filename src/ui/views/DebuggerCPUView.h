#pragma once

#include <imgui.h>
#include <cstdint>
#include <functional>

#include "../../core/AmigaEmulator.h"
#include "../../core/CpuExtState.h"
#include "AddressActions.h"

namespace adm::ui {

class EmulatorView;

class DebuggerCPUView
{
public:
    explicit DebuggerCPUView(EmulatorView* emulatorView);

    void Draw();
    void Clear();
    bool* GetOpenPtr() { return &m_open; }
    void Restore(bool open);

    // While true, Draw() shows a placeholder instead of live content — see DebuggerView::Draw().
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }

    // Show captured register state from a trace entry instead of live CPU state
    void SetTraceEntry(bool active, const adm::AmigaEmulator::CpuTraceEntry& entry);

    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }
    std::function<void()> m_onStepFrame;

private:
    EmulatorView* m_emulatorView;
    const AddressActionContext* m_addrCtx = nullptr;
    bool m_open = false;
    bool m_noFocus = false;
    bool m_turboSuppressed = false;

    // Trace mode state
    bool m_traceMode = false;
    adm::AmigaEmulator::CpuTraceEntry m_traceEntry{};

    // Last register snapshot captured while paused. Held (greyed) while the CPU is
    // free-running so the table shows the break point's values instead of churning
    // through unreadable live state; refreshed the moment execution stops.
    adm::AmigaEmulator::CpuState m_lastState{};
    bool m_haveLastState = false;
    // The same, for the stack/control/MMU/FPU registers. Also remembers the CPU model, so
    // trace mode can still grey what the machine lacks.
    adm::core::CpuExtendedState m_lastExt{};
    bool m_haveLastExt = false;

    void DrawContent();

    // Everything below the toolbar. ext is null when the extended registers can't be shown
    // (trace mode, an older core); extNote then says why. cpuModel/fpuModel still decide what
    // is greyed as absent from this machine.
    struct Panel
    {
        const uint32_t* d = nullptr;
        const uint32_t* a = nullptr;
        uint32_t pc = 0;
        uint16_t sr = 0;
        bool valid = false;       // D/A/PC/SR hold real values
        bool interactive = false; // values are live links
        const adm::core::CpuExtendedState* ext = nullptr;
        const char* extNote = nullptr;
        int cpuModel = 68000;
        int fpuModel = 0;
        int addressBits = 24;
    };
    void DrawPanel(const Panel& p);
    void DrawModelStrip(const Panel& p);
    void DrawStatus(const Panel& p);
    void DrawStack(const Panel& p);
    void DrawExtGroup(const Panel& p, adm::core::CpuRegGroup group, const char* title);
    void DrawFpu(const Panel& p);
    void DrawExtValue(const Panel& p, const adm::core::CpuRegDef& def);
    void ExtTooltip(const Panel& p, const adm::core::CpuRegDef& def, bool present);

    // Render a register value as a clickable memory hyperlink (matching the app-wide
    // link style). When not interactive (e.g. while the CPU free-runs) it falls back to
    // plain text so the held snapshot reads as inert. hexDigits is 8 for D/A/PC, 4 for SR.
    void DrawRegLink(const char* id, uint32_t value, int hexDigits, bool interactive);
};

} // namespace adm::ui
