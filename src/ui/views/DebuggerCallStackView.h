#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../../core/CallStackProfiler.h" // CallFrame
#include "../../core/StackUnwinder.h"

namespace adm::ui {

/**
 * @brief A debugger window showing the call stack, from either of two sources.
 *
 * **Live** unwinds the halted CPU's real stack with DWARF call-frame info, which is what
 * gdb does when VS Code asks it for a backtrace. It answers "where am I now", works at any
 * breakpoint or single step, and needs no CPU trace.
 *
 * **Trace** takes a vertical slice through the Frame Scrubber's reconstructed flame chart
 * at the playhead. It answers a different question -- "who was executing at this raster
 * position" -- over a whole recorded frame, including code the live view can never catch
 * because it has already returned.
 *
 * Auto picks Live whenever the machine is paused and Trace while it runs, which is almost
 * always what is wanted; the mode can be pinned when it is not.
 */
class DebuggerCallStackView
{
public:
    enum class Mode { Auto, Live, Trace };

    /// Why the trace-derived stack has nothing to show. The old single message covered
    /// all of these and so explained none of them.
    enum class TraceState {
        Ok,
        NoSymbols,         // no sibling .elf
        NoCpuTrace,        // CPU tracing is off, so no frame was ever reconstructed
        NoSegments,        // symbols exist but the program's load address is unknown
        NothingAtPlayhead, // the playhead is over untraced code (ROM, a library, a gap)
    };

    void Draw();
    void Clear()
    {
        m_frames.clear();
        m_live = {};
        m_haveLive = false;
    }

    bool* GetOpenPtr() { return &m_open; }
    bool IsOpen() const { return m_open; }
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }
    void Open() { m_open = true; }
    void Restore(bool open)
    {
        m_open = open;
        if (open)
            m_noFocus = true;
    }

    /// Fed each frame before Draw(): the outermost-first trace stack and its playhead.
    void SetStack(std::vector<adm::core::CallFrame> frames, uint32_t lRef, TraceState state)
    {
        m_frames = std::move(frames);
        m_lRef = lRef;
        m_traceState = state;
    }

    /// Fed each frame before Draw() when the machine is halted. `region` names the memory
    /// area the unwind stopped in, for the terminal row.
    void SetLiveStack(adm::core::UnwindResult result, std::string region)
    {
        m_live = std::move(result);
        m_liveRegion = std::move(region);
        m_haveLive = true;
    }
    void ClearLiveStack() { m_haveLive = false; }

    /// Jump the Disassembly view to a frame's address when its row is clicked.
    void SetOnNavigateToAddress(std::function<void(uint32_t)> cb) { m_onNavigate = std::move(cb); }
    /// Open a source file at a line, when the row has one and an editor is configured.
    void SetOnOpenSource(std::function<void(const std::string&, uint32_t)> cb)
    {
        m_onOpenSource = std::move(cb);
    }

private:
    void DrawLive();
    void DrawTrace();
    [[nodiscard]] bool ShowLive() const;

    bool m_open = false;
    bool m_noFocus = false;
    bool m_turboSuppressed = false;
    Mode m_mode = Mode::Auto;

    // Trace-derived (frame scrubber playhead).
    std::vector<adm::core::CallFrame> m_frames; // outermost first
    uint32_t m_lRef = 0;
    TraceState m_traceState = TraceState::NoSymbols;

    // Live (DWARF unwind at the halted PC).
    adm::core::UnwindResult m_live;
    std::string m_liveRegion;
    bool m_haveLive = false;

    std::function<void(uint32_t)> m_onNavigate;
    std::function<void(const std::string&, uint32_t)> m_onOpenSource;
};

} // namespace adm::ui
