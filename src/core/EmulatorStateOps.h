#pragma once

#include <string>

#include "EmulatorStateStore.h"

namespace adm {
class AmigaEmulator;
}

namespace adm::core {

/**
 * @brief Save/restore of whole machine snapshots, shared by the MCP tools and the console.
 *
 * The store itself only keeps bytes. Deciding whether a snapshot may be taken or applied
 * needs the emulator too — is a game loaded, does the core build match, was the same
 * document mounted — and those checks are the whole value of the operation: applying a
 * snapshot from a different core or disk yields a machine that is broken in ways which are
 * very hard to read back from the debugger. Keeping them here means the console and the
 * MCP tools cannot drift into disagreeing about what is safe.
 */
struct StateOpResult
{
    bool ok = false;
    std::string error;   // why nothing happened; empty when ok
    std::string warning; // it happened, but with a caveat worth reporting
    EmulatorStateStore::StateInfo info;
};

// Snapshot the running machine under `name`. `document` is the path of the currently
// mounted document, recorded so a later restore can refuse a mismatched disk. With
// `persist`, the snapshot is also written to the store's directory; a disk failure there
// is reported as a warning, not an error, because the in-memory copy is still usable.
StateOpResult SaveEmulatorState(AmigaEmulator& emu, EmulatorStateStore& store,
                                const std::string& name, const std::string& document, bool persist);

// Restore the snapshot called `name` onto the running machine.
StateOpResult LoadEmulatorState(AmigaEmulator& emu, EmulatorStateStore& store,
                                const std::string& name, const std::string& document);

} // namespace adm::core
