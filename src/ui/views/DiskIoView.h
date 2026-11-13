#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <imgui.h>

#include "../../core/DiskProfiler.h"

namespace adm::ui {

// Session log of the emulated program's own floppy DMA traffic — the counterpart to the
// Blitter Operations window. Answers "did this program write anything back to the disk,
// and where did every byte it read come from?" without leaving the debugger.
class DiskIoView
{
public:
    explicit DiskIoView(adm::core::DiskProfiler* profiler);

    void Open() { m_isOpen = true; }
    void Restore()
    {
        m_isOpen = true;
        m_noFocusOnAppearing = true;
    }
    void Close() { m_isOpen = false; }
    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    void Draw();

    // Fired when the user clicks a hyperlinked DSKPT — opens the Memory window.
    std::function<void(uint32_t addr, uint32_t len)> m_onJumpToMemory;

    // Fired when the user clicks a hyperlinked arming PC — opens the Disassembly window.
    std::function<void(uint32_t addr)> m_onJumpToDisasm;

    // Fired when the user clicks a row — propagates the L-Ref for frame-scrubber sync.
    std::function<void(uint32_t lRef)> m_onJumpToLRef;

private:
    adm::core::DiskProfiler* m_profiler = nullptr;

    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;

    int m_selectedIdx = -1; // index into the profiler's record deque, -1 = none
    bool m_followTail = true;
    bool m_writesOnly = false;

    void DrawStatsBar(const adm::core::DiskProfiler::Stats& stats);
    void DrawTable();
    void DrawDetail(const adm::core::DiskRecord& rec);
};

} // namespace adm::ui
