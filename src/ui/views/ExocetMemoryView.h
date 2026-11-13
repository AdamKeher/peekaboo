#pragma once

#include <cstdint>

#include <imgui.h>

namespace adm::core {
class ExocetSession;
}

namespace adm::ui {

// Exocet Memory: the running disk's memory map - every
// allocation its programs made, named, laid over exec's free list - as a bar a memory region and a
// table. It reads what core::ExocetSession::Memory() gives it; a disk without a map shows the same
// interface, empty and disabled.
class ExocetMemoryView
{
public:
    explicit ExocetMemoryView(core::ExocetSession* session) : m_session(session) {}

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
    // What the window holds, in the current window: the Exocet video draws it in its own panel,
    // the bars without the table (`table` false).
    void DrawContents(bool table = true);

private:
    core::ExocetSession* m_session;
    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;
    uint32_t m_selected = 0; // the allocation picked in the table or a bar (its address)
    uint32_t m_hovered = 0;  // ... and the one under the mouse last frame, outlined in the bar
};

} // namespace adm::ui
