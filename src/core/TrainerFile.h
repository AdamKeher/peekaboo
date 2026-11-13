#pragma once

#include <optional>
#include <string>
#include <vector>

#include "MemoryLocks.h"

namespace adm::core {

/**
 * @brief Which releases a trainer file claims to work with.
 *
 * A single MD5 is too brittle on its own: the same game ships as a dozen cracks from
 * different release groups, each with a different hash and often a different code layout.
 * So a file can list several hashes it has been verified against, and fall back to the TOSEC
 * title group (IdentifyResult::tosecTitleGroupId) to *offer* itself for a release nobody has
 * verified yet. That fallback is a candidacy hint, not a promise — which is precisely why
 * every entry carries an anchor and guard bytes rather than a bare address.
 */
struct TrainerIdentity
{
    std::vector<std::string> md5;  ///< Media hashes this file is known to work with.
    std::string tosecTitleGroupId; ///< "same title, different release" fallback key.

    // Advisory only: the machine the trainer was authored on. A mismatch is the usual reason
    // an absolute anchor stops resolving, so it is worth warning about rather than enforcing.
    std::string kickstart;
    uint32_t chipRamKb = 0;
    uint32_t fastRamKb = 0;

    bool Empty() const { return md5.empty() && tosecTitleGroupId.empty(); }
};

/// How a trainer file came to be associated with what is loaded.
enum class TrainerMatch
{
    None,       ///< Opened by hand.
    ExactMd5,   ///< The loaded media's hash is listed in the file.
    TitleGroup, ///< Same TOSEC title, but a release the author never verified.
};

const char* ToString(TrainerMatch m);

/// A whole trainer document: metadata plus the entries, which are ordinary LockEntry values.
struct TrainerDoc
{
    int version = 1;
    std::string title;
    std::string author;
    std::string notes;
    TrainerIdentity identity;
    std::vector<LockEntry> entries;
};

/**
 * @brief Reading and writing trainer files.
 *
 * The native format is our own JSON — no existing format fits. RetroArch's `.cht` has no
 * block mode, no byte patches, no grouping and no identity metadata (and libretro-database
 * carries no Amiga cheats at all, because P-UAE never implemented the cheat API), while
 * Cheat Engine's `.CT` is Windows-process-centric XML. Importing `.cht` is still worth it for
 * the hand-written files that do exist.
 */
class TrainerFile
{
public:
    /// Reads a peekaboo-trainer JSON file. Returns nullopt and fills err on failure.
    static std::optional<TrainerDoc> Load(const std::string& path, std::string& err);

    /// Writes a peekaboo-trainer JSON file.
    static bool Save(const std::string& path, const TrainerDoc& doc, std::string& err);

    /**
     * @brief Imports a RetroArch/libretro `.cht` file.
     *
     * Maps desc/enable/address/value/memory_search_size onto our entries, and turns
     * repeat_count + repeat_add_to_address into one entry per stride. Everything RetroArch
     * carries that we have no equivalent for — the rumble keys, `code`, and any
     * `cheat_type` other than 1 ("set to value") — is dropped, and `skipped` reports how
     * many entries that cost so the user is told rather than left wondering.
     *
     * Addresses become absolute anchors with no guard bytes, because a .cht has nothing to
     * derive either from. They are therefore exactly as brittle as the .cht was.
     */
    static std::optional<TrainerDoc> ImportCht(const std::string& path, std::string& err,
                                               int* skipped = nullptr);

    /// True when `doc` claims the given media hash outright.
    static bool MatchesMd5(const TrainerDoc& doc, const std::string& md5);

    /**
     * @brief Looks for a trainer file matching the loaded media.
     * @param dir Folder to search (typically "trainers").
     * @param md5 Media hash; may be empty.
     * @param titleGroupId TOSEC title-group id; may be empty.
     * @param outMatch How the returned file was matched.
     * @return Path to the best match, or empty. An exact hash always beats a title group.
     */
    static std::string FindForMedia(const std::string& dir, const std::string& md5,
                                    const std::string& titleGroupId, TrainerMatch& outMatch);
};

} // namespace adm::core
