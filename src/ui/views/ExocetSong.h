#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

namespace adm::core {
class ExocetSession;
struct ExocetCheck;
} // namespace adm::core

namespace adm::ui::exo {

// A thing the Exocet windows say about the song: its label, its value, what hovering it says, and
// a colour where the value is a verdict
struct SongFact
{
    std::string label, value, tip;
    std::optional<ImVec4> colour;
};

// The song as the pack names it and the kit's check gives it: its module (each song's, where there
// are several), length, played rows, positions, samples, and whether the keys were placed on this
// order list - "-" for what the check has not given yet. The Exocet window's Song card, and the
// Exocet Music window's song line.
std::vector<SongFact> SongFacts(const core::ExocetSession& session);

// What hovering position `pos` of `song` says: its pattern, when it first plays (at `fps`), and the
// other positions that play the pattern; the lead's, for a negative one
std::string PositionTip(const core::ExocetCheck& check, size_t song, int pos, int fps);

// The song's order list as a tracker's order row shows it, a line across the window: a cell a
// position with the pattern it plays, `playRow`'s position lit and the others that play its
// pattern tinted, where the song is in front - with no playhead (-1), the song the samples are of.
// Hovering a cell says when its position plays, at `fps`. The Exocet video's grid, and the Exocet
// Music window.
void DrawOrderList(const core::ExocetCheck& check, int playRow, int fps);

} // namespace adm::ui::exo
