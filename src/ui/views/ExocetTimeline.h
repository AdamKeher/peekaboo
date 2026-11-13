#pragma once

#include <cstdint>
#include <functional>

namespace adm::core {
class ExocetSession;
struct ExocetBlockState;
} // namespace adm::core

namespace adm::ui::exo {

// The demo across the song and past it: a lane a part -
// what was on the screen, and what the disk did for it, by phase - and a lane a group - the rows it
// ran on, its events' keys - with `block`'s playhead and the time under them. The Exocet window's,
// and the Exocet video's, which passes `fill` to stretch the lanes over the room left in its panel.
// The grid's breakpoints show on it, and a double-click on a row calls `onShowRow` with it, if
// given. `recordedRange`, if given, is the frame history's machine frames (false: none): the song
// frames it holds show as a band along the time axis. With `onSeekFrame` too, the playhead can be
// dragged through them, the machine frame under it sought as it goes; with `drawPreview`, a tooltip
// over them has the screen of the machine frame on its left (`drawPreview` draws it, false for
// none).
void DrawTimeline(core::ExocetSession& session, const core::ExocetBlockState& block,
                  bool fill = false, const std::function<void(int row)>& onShowRow = {},
                  const std::function<bool(uint64_t& first, uint64_t& last)>& recordedRange = {},
                  const std::function<void(uint64_t frame)>& onSeekFrame = {},
                  const std::function<bool(uint64_t frame)>& drawPreview = {});

} // namespace adm::ui::exo
