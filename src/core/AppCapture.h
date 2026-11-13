#pragma once

#include <chrono>
#include <mutex>
#include <optional>

#include "UiLayoutSnapshot.h"

namespace adm::core {

// Captures Peekaboo's own window - the whole application, or one window's rectangle out of it -
// as a PNG.
//
// This cannot be done where the MCP tools run. They execute at the top of Application::RunFrame,
// which is inside the ImGui frame but before anything has been drawn; the default framebuffer at
// that moment holds a just-swapped back buffer whose contents are undefined. So a request is
// parked here and serviced from main.cpp immediately before glfwSwapBuffers, where the pixels
// about to be presented are the pixels on screen. The requesting MCP tool parks on
// McpServer::kRetryKey until Take() yields the result.
//
// Contrast with AmigaEmulator::CaptureScreenshotPNG, which reads the emulator's own GL texture:
// that has no ImGui chrome, is at Amiga resolution rather than window resolution, and is already
// top-row-first. A framebuffer read is bottom-row-first and has to be flipped.
class AppCapture
{
public:
    // Queue a capture. Returns false when one is already in flight - the tools serialise on this
    // so two callers cannot interleave and collect each other's images.
    bool Request(const UiCaptureRequest& req);

    bool HasPending() const;

    // Called on the main thread with the GL context current, after the final draw and before the
    // buffer swap. `fbW/fbH` come from glfwGetFramebufferSize; `displayW/displayH` are
    // io.DisplaySize, and the ratio between them is the DPI scale applied to the requested rect.
    void Service(int fbW, int fbH, float displayW, float displayH);

    // Collect a finished capture, clearing it. Empty while the request is still pending.
    //
    // Also enforces the deadline: McpServer re-queues a kRetryKey result forever, so a request that
    // is never serviced - a minimised window, a stalled render loop - would park an HTTP worker and
    // then spin on a promise nobody is waiting for. After kDeadline a pending request is abandoned
    // and surfaces here as a failed result, which ends the tool's retry loop.
    std::optional<UiCaptureResult> Take();

    // Comfortably inside McpServer::kRequestTimeoutSeconds (10s), matching how run_until_frame
    // budgets its own wait.
    static constexpr std::chrono::seconds kDeadline{8};

private:
    mutable std::mutex m_mutex;
    std::optional<UiCaptureRequest> m_pending;
    std::optional<UiCaptureResult> m_result;
    std::chrono::steady_clock::time_point m_requestedAt;
};

} // namespace adm::core
