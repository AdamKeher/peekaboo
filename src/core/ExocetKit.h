#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ExocetEval.h"

namespace adm::core {

class Subprocess;

// Running a production's Exocet kit. The crass repo's
// docs/exocet.md, "The kit", has the contract. Peekaboo never reimplements the music
// export, the packer or the disk layout: it runs the kit's commands by role, each in its own
// process, and reads their reports. What a report's issues say is the kit's business; their shape
// is shared.

// One entry of a report's `issues` - or one of Peekaboo's own, in the same shape.
struct ExocetIssue
{
    std::string level; // "block", "warn", "info"
    std::string code;  // stable, namespaced ("music.arena", "exocet.range", "tool.lz4", ...)
    std::string title;
    std::string text;
    nlohmann::json at;      // {pos, row, pass}, or null
    nlohmann::json subject; // {sample, name} / {track}, or null
    std::vector<std::string>
        blocks;            // for a block: the actions it stops ("push", "inject", "write")
    bool peekaboo = false; // Peekaboo's finding, not the kit's

    bool Blocks(const std::string& action) const;
    nlohmann::json ToJson() const;
};

std::vector<ExocetIssue> ExocetParseIssues(const nlohmann::json& report);
ExocetIssue ExocetMakeIssue(const std::string& level, const std::string& code,
                            const std::string& title, const std::string& text,
                            std::vector<std::string> blocks = {});

// kit.json.
struct ExocetKitInfo
{
    std::string path; // kit.json
    std::string dir;
    std::string production;
    std::string paramsPath;                                   // resolved
    std::map<std::string, std::string> requirements;          // kit.json's "requires"
    std::map<std::string, std::vector<std::string>> commands; // role -> script, fixed arguments

    bool Load(const std::string& kitJson, std::string* error = nullptr);
    bool HasRole(const std::string& role) const { return commands.count(role) != 0; }
};

// What a kit is, for trusting it: MD5 over every file in its folder (sorted by relative path,
// __pycache__ left out), each as its path, a NUL, and its bytes. Empty if the folder is unreadable.
// `skip` names files (relative paths) that are data, not code - the kit's params.json, which the
// build rewrites for every disk and which nothing runs - so a rebuilt pack is not asked about
// again.
std::string ExocetKitHash(const std::string& dir, const std::vector<std::string>& skip = {});

// The Python and the tools directory kit commands run with.
struct ExocetTools
{
    std::string python;   // an executable; "python" looks it up on PATH
    std::string toolsDir; // EXOCET_TOOLS; empty for none
};

// What those actually are.
struct ExocetToolVersions
{
    bool pythonRuns = false;
    std::string python;   // "3.12.10"
    std::string lz4;      // the lz4 package's __version__, empty if it will not import
    std::string salvador; // "1.4.1", empty if there is none in the tools directory
    std::string error;
};

ExocetToolVersions ExocetProbeTools(const ExocetTools& tools);

// kit.json's `requires` against the versions: a block issue (`tool.*`) for each unmet one, naming
// the tool and both versions. "python" takes ">=X.Y"; the rest must be equal.
std::vector<ExocetIssue> ExocetCheckRequires(const ExocetKitInfo& kit, const ExocetToolVersions& v);

struct ExocetKitResult
{
    bool launched = false;
    bool timedOut = false;
    int exitCode = -1;
    std::string output; // stdout and stderr
    nlohmann::json report;
    std::vector<ExocetIssue> issues;
    double seconds = 0;

    bool Ok() const;
};

// Runs `role` with `args` (the report path is appended) in a scratch directory, with EXOCET_TOOLS
// set and PYTHONPATH cleared, killed after `timeoutMs`. A command that fails without writing its
// report gets a block issue carrying the end of its output. `proc` lets another thread cancel.
// Its folder is made, and removed after, under `scratchRoot` (ExocetScratchDir), or the system's
// temp folder when that is empty.
ExocetKitResult ExocetRunKit(const ExocetKitInfo& kit, const ExocetTools& tools,
                             const std::string& role, const std::vector<std::string>& args,
                             int timeoutMs = 120000, Subprocess* proc = nullptr,
                             const std::string& scratchRoot = {});

// Where the kit's work files go for the pack at `exocetPath`: exocet_temp in its folder, beside
// the .exocet and the disk - not the system's temp folder, whose cleaners deleted it under a
// running session. The system's temp folder when there is no pack.
std::string ExocetScratchDir(const std::string& exocetPath);

// The check report: the rows as played, with their cells, the samples, the resolved keys. A
// production may play several songs one after another (the .exocet's "songs": the Sneezeman
// disk's intro then its main song); the rows are theirs end to end, each with its song, and a
// song's lead - the kit's silent rows before the musician's - has negative positions.
struct ExocetRow
{
    uint16_t song = 0;
    int16_t pos = 0;
    uint16_t pattern = 0, row = 0, pass = 0;
    uint32_t frame = 0;
    uint16_t frames = 0;
    // per channel: sample, period, effect, parameter
    std::vector<std::array<uint16_t, 4>> cells;
};

struct ExocetSampleInfo
{
    int index = 0;
    std::string name;
    uint32_t bytes = 0;
    int volume = 0;
    bool loop = false;
    // How the kit packs it (the crass repo's docs/exocet.md, "The music"), for a sample with data:
    // its packed 8-bit size, its 4-bit size, how close the 4-bit one comes (dB, and the worst
    // single error in levels), the .exocet's setting, the codec the kit chose and why
    bool assessed = false;
    uint32_t packed = 0, delta4 = 0;
    double db = 0;
    int worst = 0;
    std::string setting; // "auto", "8bit", "4bit"
    std::string codec;   // "8bit", "4bit"
    std::string why;
    bool used = false; // the song plays it
};

struct ExocetCheck
{
    bool valid = false;
    std::vector<ExocetRow> rows;
    std::vector<ExocetSampleInfo> samples;
    std::map<std::string, std::vector<ExocetKey>> keys; // resolved, by track
    std::vector<int> order; // the paged song's (the one with the samples' report)
    int channels = 4;
    uint32_t digest = 0; // the first song's: the one exocet.bin's header names
    // each song's order list and trigger digest, in play order
    std::vector<std::vector<int>> orders;
    std::vector<uint32_t> digests;
    int musicSong = -1; // the song the samples are of: the paged one (-1: not said)
    uint32_t frames = 0;
    nlohmann::json music, exocet;
    std::vector<ExocetIssue> issues;

    bool Parse(const nlohmann::json& report);
    // The played row at (song, position, row, pass); -1 if the song does not play it.
    int FindRow(int song, int pos, int row, int pass) const;
    // The played row a song frame is in.
    int RowAtFrame(uint32_t frame) const;

private:
    static uint64_t Place(int song, int pos, int row, int pass);
    std::map<uint64_t, int> m_index;
};

// A played row's place as the grid shows it: "pp:rr", ".n" after it for a pass past the first,
// the song's number in front ("1 ") when the check has more than one, and a lead position (the
// kit's silent rows before the musician's) as "L1" for -1, "L2" for -2.
std::string ExocetPlaceLabel(const ExocetRow& r, bool songs);
// The label read back: "pp:rr", "pp:rr.n", "L1:rr", with the song in front as "s pp:rr" or
// "s:pp:rr" (else `song` is left as it is). Decimal throughout. False when it is not a place.
bool ExocetParsePlace(const std::string& text, int* song, int* pos, int* row, int* pass);

// "C-3", "---" for no note: a ProTracker period as a tracker shows it.
std::string ExocetNoteName(uint16_t period);

} // namespace adm::core
