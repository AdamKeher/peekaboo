#pragma once

#include <imgui.h>

namespace adm::core {
class ExocetSession;
}

namespace adm::ui {

// Exocet Music: how each of the module's
// samples goes onto the disk - packed 8-bit, or 4-bit deltas at half the size and a measured
// loss - as the .exocet's music section says and the kit's check reports, with the song and each
// sample to listen to both ways. Over them, the song: the Exocet window's Song card, and its order
// list as the Exocet video's grid has it. It edits the pack (core::ExocetSession), so a change is
// undone, saved and checked like a key.
class ExocetMusicView
{
public:
    explicit ExocetMusicView(core::ExocetSession* session) : m_session(session) {}

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

private:
    void DrawSong();

    core::ExocetSession* m_session;
    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;
    bool m_showUnused = false; // samples the song never plays
};

} // namespace adm::ui
