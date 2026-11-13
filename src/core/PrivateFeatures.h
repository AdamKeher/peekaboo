#pragma once

namespace adm::core {

// Exocet (the sync tracker) and the crAss engine's own tools are made for productions that are
// not public, so a plain Peekaboo hides them: no menu, windows, console command, MCP tools or
// overlay rows. user_prefs.json's "privateFeatures": true, or --private-features for one launch,
// shows them. Settled once at startup, before any view or tool list is built; nothing follows a
// change made while running.
inline bool& PrivateFeaturesFlag()
{
    static bool on = false;
    return on;
}

inline bool PrivateFeatures() { return PrivateFeaturesFlag(); }

inline void SetPrivateFeatures(bool on) { PrivateFeaturesFlag() = on; }

} // namespace adm::core
