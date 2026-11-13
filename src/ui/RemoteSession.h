#pragma once

struct GLFWwindow;

namespace adm::ui {

// Remote Desktop helpers for the emulator's mouse capture. Kept out of the views so they need not
// include <windows.h>, whose A/W macros rename members such as GetSystemDirectory.

// True when this process is running in a Remote Desktop session. Asked at the point of use rather
// than cached: a running session can be taken over by an RDP connection, and handed back.
bool IsRemoteSession();

// Confine the cursor to the window's client area, or lift that again. RDP input is absolute, so
// the clip is what keeps a hidden captured cursor (and its clicks) inside the window and parks
// it on the border, where the capture notices it and recentres. Re-applied every frame, which
// also follows the window when it is moved or resized.
void ClipCursorToWindow(GLFWwindow* window);
void ReleaseCursorClip();

} // namespace adm::ui
