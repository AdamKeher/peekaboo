#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "CrassMemMap.h"
#include "ExocetBlock.h"

namespace adm::core {

class ExocetPack;
class ExocetSession;
struct ExocetCheck;

// What the Exocet MCP tools and the console's EXO command both answer:
// each function takes the session and the tool's arguments and returns the tool's JSON, or
// {"error": why}. The MCP handler returns it as it is; the console prints it as text. Nothing
// here is worked out twice.
//
// A place in the song, in any function that takes one: `playedRow` (the check's index), `frame`
// (a song frame: the row it is in), `place` (the grid's label, "05:10", "05:10.1", "L1:00",
// "1 05:10"), `song`/`pos`/`row`/`pass` (song and pass default 0), or `bookmark` (a slot 1-9 or
// a bookmark's name).

// The state get_exocet reports: the running program's block and the sync pack beside the active
// document, which are from the same build only when their manifest hashes agree.
struct ExocetSnapshot
{
    ExocetBlockState block;
    std::shared_ptr<const ExocetPack> pack; // null: no `.exocet` beside the document
    std::string packError;                  // why a `.exocet` that was found did not load
    std::string documentPath;
    // which groups the demo had enabled on each played row (ExocetActivity::ToJson); null: none
    nlohmann::json activity;
};

// The played row `args` names (see above); -1 with `error` set when it names none the song plays.
// `required` false: no place given at all is -2, not an error.
int ExocetResolvePlace(const ExocetSession& s, const nlohmann::json& args, std::string* error,
                       bool required = true);
// A played row as its place: song, pos, row, pass, playedRow, frame, label. Null off the rows.
nlohmann::json ExocetPlaceJson(const ExocetCheck& check, int playedRow);

// ---- reading ----
nlohmann::json ExocetStatusJson(const ExocetSnapshot& snap); // get_exocet
nlohmann::json ExocetMemoryJson(const ExocetMemoryState& m, const nlohmann::json& args);
nlohmann::json ExocetTimelineJson(ExocetSession& s, const nlohmann::json& args);
nlohmann::json ExocetGridJson(ExocetSession& s, const nlohmann::json& args);
nlohmann::json ExocetKeysJson(ExocetSession& s, const nlohmann::json& args);
nlohmann::json ExocetMusicJson(ExocetSession& s, const nlohmann::json& args);
nlohmann::json ExocetDiskMapJson(ExocetSession& s, const nlohmann::json& args);
nlohmann::json ExocetProofJson(ExocetSession& s);
nlohmann::json ExocetIssuesJson(ExocetSession& s);

// ---- editing ----
nlohmann::json ExocetSetKeyJson(ExocetSession& s, const nlohmann::json& args, bool remove);
nlohmann::json ExocetSetKeysJson(ExocetSession& s, const nlohmann::json& args);
nlohmann::json ExocetEveryHitJson(ExocetSession& s, const nlohmann::json& args);
// Every key on `track` removed, one undo step (the grid's Clear); `save` writes the .exocet
nlohmann::json ExocetClearTrackJson(ExocetSession& s, const nlohmann::json& args);
// "undo", "redo" or "save"
nlohmann::json ExocetEditJson(ExocetSession& s, const std::string& action);
nlohmann::json ExocetCheckJson(ExocetSession& s, const nlohmann::json& args);

// ---- the machine ----
nlohmann::json ExocetPushJson(ExocetSession& s);
nlohmann::json ExocetInjectJson(ExocetSession& s, const nlohmann::json& args);
nlohmann::json ExocetWriteJson(ExocetSession& s, const nlohmann::json& args);
// `seek` is the frame history's (an error string, empty when it went there); null: none.
nlohmann::json ExocetSeekJson(ExocetSession& s, const nlohmann::json& args,
                              const std::function<std::string(uint64_t)>& seek);
nlohmann::json ExocetBreakpointsJson(ExocetSession& s, const nlohmann::json& args);
// The .exocet's bookmarks: list, add (a place; `slot`, `name`), remove, clear
nlohmann::json ExocetBookmarksJson(ExocetSession& s, const nlohmann::json& args);

} // namespace adm::core
