#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "FrameInput.h"

namespace adm::core {

// One piece of media the scenario expects to find mounted. Identity is the content MD5, in
// the spelling DiskIdentifier already produces: a replay against different media cannot
// mean anything, so it is refused rather than run and reported.
struct ScenarioMedia
{
    unsigned drive = 0;
    std::string name;
    uint64_t size = 0;
    std::string md5;
};

// Everything needed to decide whether replaying this scenario means anything, and to
// reproduce the machine it was recorded on.
struct ScenarioManifest
{
    std::string created;         // ISO-8601 UTC
    std::string coreVersion;     // adm_get_version() at record time
    std::string appCommit;       // short git hash, best effort, may be empty
    std::string model;           // model preset id, e.g. "a500"
    std::string videoStandard;   // "PAL" / "NTSC"
    std::string videoResolution; // "Lores" / "Hires" / "SuperHires"
    std::map<std::string, std::string> userVariables;
    std::vector<ScenarioMedia> media;
    uint64_t frameCount = 0;
    bool coldBoot = true; // false = the SEED chunk carries a save state
    std::string notes;
};

// What one frame is expected to look like. `audioSamples` is kept out of the audio hash on
// purpose: a fault that changes how many samples a frame produces - one write to $DFF02A
// halves PuAE's rate permanently - is worth reporting as itself rather than as an opaque
// digest mismatch.
struct ScenarioFrameDigest
{
    uint64_t video = 0;
    uint64_t audio = 0;
    uint32_t audioSamples = 0;

    bool operator==(const ScenarioFrameDigest&) const = default;
};

struct ScenarioRamDigest
{
    uint64_t frame = 0;
    uint64_t digest = 0;
};

// A PNG of the frame as it looked when the scenario was blessed. Sparse - one every N
// frames - because the point is to give a divergence somewhere to be looked at, not to
// store a film.
struct ScenarioKeyframe
{
    uint64_t frame = 0;
    std::vector<uint8_t> png;
};

struct ScenarioGolden
{
    std::vector<ScenarioFrameDigest> frames;
    std::vector<ScenarioRamDigest> ram;
    std::vector<ScenarioKeyframe> keyframes;

    bool Empty() const { return frames.empty(); }
    // The blessed PNG nearest to (at or before) `frame`, or nullptr when there is none.
    const ScenarioKeyframe* NearestKeyframe(uint64_t frame) const;
};

/**
 * @brief A recorded session: the machine it ran on, where it started, what was pressed, and
 *        optionally what it is expected to look like.
 *
 * Chunked so the golden table can be written by a second pass over a file that already
 * exists, and so a reader that does not care about goldens can skip them:
 *
 *   'P' 'K' 'S' 'C' | uint32 version | { char tag[4], uint64 length, bytes } ...
 *
 * MANI (JSON manifest, required), SEED (save state, absent = cold boot), INPT (frameCount x
 * FrameInput in FrameInputCodec's layout), GOLD (the digest table). Unknown chunks are
 * skipped, so a newer writer's file still loads.
 *
 * Input is indexed BY POSITION, not by frame number. Frame numbers are not dense - a frame
 * the machine never completed leaves a hole, a mid-frame breakpoint stop being the usual
 * cause - and indexing by number would silently desynchronise a replay one frame in.
 */
class ScenarioFile
{
public:
    static constexpr uint32_t kVersion = 1;
    // Keyframe PNGs and RAM digests are sampled this often unless told otherwise.
    static constexpr uint64_t kDefaultKeyframeInterval = 50;

    ScenarioManifest manifest;
    std::vector<uint8_t> seedState; // empty = cold boot
    std::vector<FrameInput> input;
    ScenarioGolden golden;

    std::vector<uint8_t> Serialize() const;
    // Replaces everything held. On failure `error` says what was wrong and the object is
    // left empty rather than half-parsed.
    bool Deserialize(const uint8_t* data, size_t size, std::string& error);

    bool Load(const std::filesystem::path& path, std::string& error);
    bool Save(const std::filesystem::path& path, std::string& error) const;

    void Clear();

    // Whether `other`'s machine could produce this scenario's frames at all: same model,
    // video standard and resolution, and the same media in the same drives. The core
    // version is deliberately NOT part of this - a digest mismatch after a core rebuild is
    // the finding, not an error.
    bool MachineMatches(const ScenarioManifest& other, std::string& why) const;
};

} // namespace adm::core
