#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "AnchorResolver.h"
#include "LaunchOptions.h"

namespace adm::core {

class ArMemoryAccess;

/// What an entry does to the running machine.
enum class LockMode {
    Block, ///< Drop guest writes to the range entirely.
    Set,   ///< Force a value: guest writes still land, rewritten to `value`.
    Patch  ///< One-shot byte blob written on enable, reverted on disable.
};

const char* ToString(LockMode m);
std::optional<LockMode> LockModeFromString(const std::string& s);

/**
 * @brief One protection entry, as stored and as shown in the Trainer window.
 *
 * Block and Set are enforced inside the core's memory write path (see
 * patches/adm_protect.inc), so they hold sub-frame rather than being a once-per-frame clamp.
 * Patch is applied host-side through ArMemoryAccess, the same way the TF/TFD console command
 * already NOPs instructions.
 */
struct LockEntry
{
    std::string id;    ///< Stable identity, generated on Add if empty.
    std::string desc;  ///< What the entry does, in the author's words.
    std::string group; ///< Optional grouping for the UI ("Player", "Weapons", ...).
    bool enabled = false;

    LockMode mode = LockMode::Block;
    Anchor anchor;
    BytePattern verify; ///< Guard bytes at the anchor site; empty means no check.

    uint32_t len = 1;   ///< Block: bytes protected.
    uint8_t size = 1;   ///< Set: width of the forced value (1/2/4).
    uint32_t value = 0; ///< Set: the forced value.

    std::vector<uint8_t> bytes;    ///< Patch: what to write.
    std::vector<uint8_t> original; ///< Patch: what was there before (persisted, so a crash
                                   ///< between enable and disable does not lose the revert).
    bool applied = false;          ///< Patch: currently written to memory.

    // --- runtime only, never serialised ---
    std::optional<uint32_t> resolved;
    ResolveState state = ResolveState::Unresolved;
    std::string note; ///< Why resolution failed, for the UI and the PR listing.

    /// Bytes this entry occupies once resolved (Block uses len, Set uses size, Patch bytes).
    uint32_t Span() const;
};

/// Emulator-independent mirror of AmigaEmulator::ProtectEntry. Kept separate so this class
/// stays free of any emulator dependency and can be unit-tested without one.
struct ProtectSpec
{
    uint32_t addr = 0;
    uint32_t len = 1;
    uint8_t mode = 0; ///< 0 = block, 1 = set (matches ADM_PROTECT_MODE_*)
    uint8_t size = 1;
    uint32_t value = 0;
};

/// Everything resolution and patching need from the outside world.
struct LockContext
{
    ArMemoryAccess* mem = nullptr;
    std::vector<AmigaSegment> segments; ///< From SegListResolver; may be empty.
};

/**
 * @brief Shared store for memory locks and patches.
 *
 * There is exactly one of these, owned by DebuggerView and handed by pointer to the console,
 * the register windows, the memory inspector and the Trainer window. That is what makes
 * "locking a register in the Custom window shows up in the PR listing, and vice versa" true
 * by construction rather than by synchronisation code.
 *
 * Modelled on MemoryAnnotations: constructed-loads, mutation-saves, backed by a JSON file in
 * the working directory.
 */
class MemoryLocks
{
public:
    MemoryLocks() { Load(); }

    // --- inspection ---
    const std::vector<LockEntry>& All() const { return m_entries; }
    size_t Size() const { return m_entries.size(); }
    bool Empty() const { return m_entries.empty(); }
    const LockEntry* Find(const std::string& id) const;
    LockEntry* Find(const std::string& id);
    /// First entry whose resolved span covers `addr`, or null. Used by the register windows
    /// and the memory inspector to show whether an address is already locked.
    const LockEntry* FindCovering(uint32_t addr) const;

    // --- mutation (each one saves and bumps the revision) ---
    /// Adds an entry, assigning an id if it has none. Returns the id.
    std::string Add(LockEntry e);
    bool Remove(const std::string& id);
    /// Removes every entry whose resolved span covers `addr`. Returns how many went.
    size_t RemoveCovering(uint32_t addr);
    void Clear();
    void ReplaceAll(std::vector<LockEntry> entries);

    /**
     * @brief Change what a Set-mode entry forces, keeping it enabled.
     *
     * This is what "type a new value and press Set" does on a register that is already
     * locked: without it the write lands once and the lock immediately forces the old value
     * back, which reads as the field resetting itself.
     *
     * Rejects a value too wide for `size` rather than truncating - a silently truncated
     * value would be forced into memory forever without the user ever seeing what they
     * actually asked for. Only meaningful for LockMode::Set.
     */
    bool SetValue(const std::string& id, uint32_t value, uint8_t size, std::string& err);

    /**
     * @brief Enable or disable an entry.
     *
     * Enabling resolves the anchor first; if it does not resolve, the entry stays disabled
     * and the reason lands in `note`. Nothing is ever written to an unverified address.
     * For Patch entries this is also where the bytes are written / reverted.
     */
    bool SetEnabled(const std::string& id, bool enabled, const LockContext& ctx, std::string& err);

    /// Re-runs resolution for every entry. Enabled entries that stop resolving are disabled.
    void ResolveAll(const LockContext& ctx);

    // --- what the core needs ---
    /// Block/Set entries that are enabled and resolved. Patch entries are not in here — they
    /// are already written to memory rather than enforced by the core.
    std::vector<ProtectSpec> BuildProtections() const;

    /// Bumped on every change. Callers re-push BuildProtections() when this moves, rather
    /// than rebuilding the core table every frame.
    uint64_t Revision() const { return m_revision; }

    // --- persistence ---
    /// Serialises entries in the shared trainer-file shape (used for both memory_locks.json
    /// and .json trainer files).
    static nlohmann::json EntryToJson(const LockEntry& e);

    /// Parses "4E714E71" / "4E 71 4E 71" into bytes. Shared by the loader and the UI so both
    /// accept exactly the same spellings.
    static std::optional<std::vector<uint8_t>> ParseHexBytes(const std::string& s,
                                                             std::string& err);
    static std::optional<LockEntry> EntryFromJson(const nlohmann::json& j, std::string& err);

    void SetPath(std::string path) { m_path = std::move(path); }
    const std::string& Path() const { return m_path; }

private:
    void Load();
    void Save() const;
    std::string MakeId();
    /// Applies or reverts a Patch entry's bytes. Fills err on failure.
    bool ApplyPatch(LockEntry& e, bool apply, const LockContext& ctx, std::string& err);

    std::vector<LockEntry> m_entries;
    uint64_t m_revision = 1;
    uint32_t m_nextId = 1;
    std::string m_path = ProfilePath("memory_locks.json").string();
};

} // namespace adm::core
