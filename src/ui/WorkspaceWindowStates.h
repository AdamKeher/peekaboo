#pragma once

#include <map>
#include <string>
#include <vector>

#include "MainLayout.h"

namespace adm::ui {

/// Sets every window's open flag from a workspace's saved window states.
///
/// A workspace is the whole arrangement: a window it does not mention is closed, not left as it
/// was. Otherwise a window added after the workspace was saved - or one the user had open in the
/// workspace before - carries over into every workspace switched to, a stray the workspace never
/// asked for. Saving a workspace writes every window, so a re-saved workspace names them all.
///
/// A window renamed in the menu keeps its old name as its ImGui id ("GIF & Video Capture" is still
/// "###GIF Capture"), and a workspace saved before the rename knows it only by that, so the id is
/// tried before deciding the workspace says nothing about a window.
inline void ApplyWorkspaceWindowStates(const std::map<std::string, bool>& saved,
                                       const std::vector<WindowState>& windows)
{
    for (const auto& window : windows) {
        if (!window.openPtr)
            continue;
        auto it = saved.find(window.name);
        if (it == saved.end() && !window.imguiId.empty())
            it = saved.find(window.imguiId);
        *window.openPtr = it != saved.end() && it->second;
    }
}

} // namespace adm::ui
