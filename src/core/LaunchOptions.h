#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace adm::core {

// Per-launch overrides from the command line, so several Peekaboos can run side by side from one
// build without sharing a port, a token or a state file. Nothing here is saved to
// user_prefs.json. Every field defaults to "not given", which is the behaviour of a plain launch:
// HTTP on PeekabooHttpServer::kPort, MCP and GDB on the ports and token the prefs hold, state
// files in the working directory (the executable's), sound as the prefs say.
//
// main() fills this in before Application is constructed, because the preferences, the HTTP
// server and the state files are all opened by Application's constructor.
struct LaunchOptions
{
    std::optional<int> httpPort;
    std::optional<int> mcpPort;
    std::optional<int> gdbPort;
    std::optional<std::string> mcpToken;
    // Where user_prefs.json, workspaces.json, the table layouts, the memory annotations and locks,
    // the Exocet activity record and saved states live. Empty = the working directory.
    std::filesystem::path profileDir;
    // No sound from the first frame: the host mute held on and Paula's channel mask held at 0,
    // whatever the prefs, the toolbar or a restart say.
    bool silent = false;
    // A machine preset id (A500, A1200, ...) selected before anything runs. Empty = the prefs'.
    std::string model;
    bool minimized = false;
    // The main window is never shown. The machine, the MCP tools and the emulator screenshot
    // still work; screenshots of the application's own windows need it visible.
    bool hidden = false;
    // Exocet and the crAss engine's tools for this launch, whatever the prefs say
    // (core/PrivateFeatures.h). Only turns them on.
    bool privateFeatures = false;
};

inline LaunchOptions& GetLaunchOptions()
{
    static LaunchOptions options;
    return options;
}

// Where a per-user state file lives: in the profile directory when one was given, else in the
// working directory as before.
inline std::filesystem::path ProfilePath(const std::filesystem::path& name)
{
    const auto& dir = GetLaunchOptions().profileDir;
    return dir.empty() ? name : dir / name;
}

} // namespace adm::core
