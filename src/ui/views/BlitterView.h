#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "../../core/BlitProfiler.h"
#include "../BlitDestPreview.h"

namespace adm::ui {

class BlitterView
{
public:
    explicit BlitterView(adm::core::BlitProfiler* profiler);

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

    // Called by DebuggerFrameScrubberView when the scrubber moves
    void SetHighlightLRef(uint32_t lRef);

    // Highlight (and scroll into view) the row matching a blit hovered in the Frame Scrubber,
    // without disturbing the click selection. UINT32_MAX = clear.
    void SetHoverHighlightLRef(uint32_t lRef) { m_hoverHighlightLRef = lRef; }

    // Fired when user clicks a row — propagates L-Ref for cross-view sync
    std::function<void(uint32_t lRef)> m_onJumpToLRef;

    // Fired when user hovers a row (UINT32_MAX = no row hovered)
    std::function<void(uint32_t lRef)> m_onHoverLRef;

    // Fired when user clicks a hyperlinked channel address — opens the Memory window
    std::function<void(uint32_t addr, uint32_t len)> m_onJumpToMemory;

    // Fired when user clicks a row's minterm value — opens the Blitter Minterm window,
    // populated from that blit's BLTCON0/BLTCON1.
    std::function<void(uint16_t bltcon0, uint16_t bltcon1)> m_onOpenMinterm;

    // Destination-data capture state, mirrored from the emulator by
    // Application::SyncBlitDestCapture. The control itself is in Debugger -> Display -> Blitter;
    // this window only needs to know whether the Data column has anything to show.
    void SetCaptureDestData(bool on) { m_captureDestData = on; }
    bool IsCaptureDestData() const { return m_captureDestData; }

    // False when the loaded core predates the capture patch, which the stats bar explains
    // rather than leaving the empty column unaccounted for.
    void SetCaptureDestDataSupported(bool supported) { m_captureSupported = supported; }

private:
    adm::core::BlitProfiler* m_profiler = nullptr;

    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;

    // Selection state
    int m_selectedIdx = -1; // index into profiler's current frame, -1 = none
    int m_lastSelectedIdx = -1;

    // Hover tracking for scrubber highlight
    uint32_t m_lastHoveredLRef = UINT32_MAX;

    // External highlight from frame scrubber (UINT32_MAX = none)
    uint32_t m_highlightLRef = UINT32_MAX;

    // Transient highlight from a Frame Scrubber blit hover (UINT32_MAX = none). Independent of the
    // click selection (m_selectedIdx); only colours the row and scrolls it into view.
    uint32_t m_hoverHighlightLRef = UINT32_MAX;
    uint32_t m_lastHoverHighlightLRef = UINT32_MAX;

    // Whether this window was hovered this frame — suppresses hover-highlight auto-scroll so the
    // Blitter window's own row-hover (which feeds back through the same channel) can't jitter it.
    bool m_windowHovered = false;

    // Destination-data ("what was blitted") preview: capture toggle and the texture cache
    // backing the Data column's thumbnails and their tooltips.
    bool m_captureDestData = false;
    bool m_captureSupported = true;
    BlitDestPreview m_destPreview;

    void DrawStatsBar(const adm::core::BlitProfiler::FrameStats& stats);
    void DrawTable(const std::vector<adm::core::BlitRecord>& records);

    static std::string FormatHex16(uint16_t v);
};

} // namespace adm::ui
