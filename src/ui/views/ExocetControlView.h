#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../FramePreview.h"

namespace adm::core {
class ExocetSession;
}

namespace adm::ui {

// The Exocet window: the pack and what the kit does with it
// - save the .exocet, check the song, push the keys into the running demo, inject the module, write
// a new disk - with the kit's trust, the tools it runs with and what the last of each did; under
// them, what the .exocet holds: the file, the song, the keys, the kit, the live block and the
// groups, laid out in as many columns as the window has room for. The timing itself is the
// Exocet Grid's (ExocetView).
class ExocetControlView
{
public:
    explicit ExocetControlView(core::ExocetSession* session) : m_session(session) {}

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

    // The Python and tools directory kit commands run with, changed in the window.
    std::function<void(const std::string& python, const std::string& tools)> m_onToolsChanged;
    std::function<std::pair<std::string, std::string>()> m_getTools;
    // Memory: open Exocet Memory
    std::function<void()> m_onOpenMemory;
    std::function<void()> m_onOpenMusic; // the Music window: samples 8-bit or 4-bit
    // The timeline's double-click: the Exocet Grid, on that played row
    std::function<void(int row)> m_onShowRow;
    // The frame history's machine frames (false: none), drawn on the timeline
    std::function<bool(uint64_t& first, uint64_t& last)> m_recordedRange;
    // The timeline's playhead, dragged through them: the frame history to that machine frame
    std::function<void(uint64_t frame)> m_onSeekFrame;
    // A machine frame's screen thumbnail, beside the timeline's tooltips over them
    FramePreview::Source m_getFrameThumbnail;

private:
    // a song frame as mm:ss.cc at the session's frame rate
    std::string Time(uint32_t frame) const;
    void DrawToolbar();
    void DrawTrustBanner();
    void DrawMessages();
    void DrawInfo();
    void DrawFileCard();
    void DrawSongCard();
    void DrawKeysCard();
    void DrawKitCard();
    void DrawLiveCard();
    void DrawGroups();

    // What the keys add up to, counted again when the pack is edited or the song checked again.
    struct Stats
    {
        struct Group
        {
            int tracks = 0, keyed = 0, keys = 0;
        };
        int tracks = 0, keyedTracks = 0, keys = 0;
        int interps[4] = {};                            // step, linear, smooth, ramp
        std::vector<std::pair<std::string, int>> types; // track type, tracks of it
        std::vector<Group> groups;                      // the manifest's order
        std::vector<std::string> orphans;               // keyed tracks the manifest no longer has
        int offSong = 0;                                // keys on no row the song plays
        int64_t firstFrame = -1, lastFrame = -1;
    };
    void Recount();

    core::ExocetSession* m_session;
    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;
    char m_python[512] = {};
    char m_tools[512] = {};

    Stats m_stats;
    FramePreview m_preview; // the timeline's tooltip's screen
    const void* m_statsPack = nullptr;
    uint64_t m_statsRevision = ~0ull, m_statsCheck = ~0ull;
};

} // namespace adm::ui
