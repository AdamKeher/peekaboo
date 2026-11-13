#include "RemoteSession.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace adm::ui {

bool IsRemoteSession()
{
#ifdef _WIN32
    return GetSystemMetrics(SM_REMOTESESSION) != 0;
#else
    return false;
#endif
}

void ClipCursorToWindow(GLFWwindow* window)
{
#ifdef _WIN32
    HWND hwnd = window ? glfwGetWin32Window(window) : nullptr;
    if (!hwnd)
        return;
    RECT rect;
    GetClientRect(hwnd, &rect);
    ClientToScreen(hwnd, reinterpret_cast<POINT*>(&rect.left));
    ClientToScreen(hwnd, reinterpret_cast<POINT*>(&rect.right));
    ClipCursor(&rect);
#else
    (void)window;
#endif
}

void ReleaseCursorClip()
{
#ifdef _WIN32
    ClipCursor(nullptr);
#endif
}

} // namespace adm::ui
