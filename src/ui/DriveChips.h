#pragma once

#include <imgui.h>
#include <string>
#include <vector>

namespace adm {
namespace ui {

// A selectable strip of drive "chips" - one per drive - shared by the Floppy Drives and Hard
// Drives windows so the two cannot drift apart again.
//
// Why this is drawn by hand rather than being an ImGuiTabBar: a tab registers its item with an
// empty rect (TabItemEx calls ItemAdd(ImRect(), ...)), so there is no bounding box to draw a
// seven-segment readout into, and a tab label is a single colour, so an inactive indicator could
// not be dimmed - it would have to be omitted, and the chip would change width.
//
// Every chip is the same width and every indicator is always drawn, dim when off. That is not
// cosmetic: a chip that resizes as its drive seeks makes the whole strip shuffle sideways under
// the cursor.

// One indicator glyph inside a chip. Always drawn; `lit` only picks the colour.
struct ChipIndicator
{
    const char* glyph = nullptr;
    bool lit = false;
    // Makes this glyph a button in its own right - the write-protect padlock is
    // one. A clickable indicator gets its own hit rect, submitted before the
    // chip's, so clicking it does not also change the drive selection.
    bool clickable = false;
    // Shown instead of the chip tooltip while the cursor is over this glyph.
    // Only useful together with `clickable`.
    std::string tooltip;
    // Colour when lit. Defaults to the shared indicator green; the padlock uses
    // amber, because a lit lock is not a healthy state, only a true one.
    ImVec4 litColor = ImVec4(0.35f, 0.80f, 0.35f, 1.0f);
};

// Which indicator on which chip the user just pressed, or {-1, -1} for none.
struct ChipIndicatorClick
{
    int chip = -1;
    int indicator = -1;
};

// Passed to `digits` when the drive has no meaningful cylinder (idle, disabled, or a backing
// that does not report one). Renders as "--" rather than "00", which would read as "parked at
// cylinder 0".
inline constexpr int kNoDigits = -1;

// Omit the seven-segment readout entirely - hard drives have no cylinder.
inline constexpr int kNoDisplay = -2;

struct DriveChip
{
    const char* name = nullptr; // "DF0", "DH0"
    bool ledOn = false;         // green vs red dot, drawn left of the name
    // What backs the drive, as a glyph in its own slot. Null leaves the slot empty but still
    // reserves its width, so chips with and without a backing stay the same size.
    const char* backingGlyph = nullptr;
    ImVec4 backingColor = ImVec4(1, 1, 1, 1);
    // Cylinder for the seven-segment readout, 0-99; kNoDigits for "--", kNoDisplay for none.
    int digits = kNoDisplay;
    // The readout doubles as the drive lamp: dim when idle, bright while reading, red while
    // writing. Same rule as the Amiga Emulator window's status bar.
    ImVec4 digitsColor = ImVec4(0.45f, 0.95f, 0.45f, 1.0f);
    std::vector<ChipIndicator> indicators;
    // Shown for the whole chip, in words - one hover explains every indicator, which beats
    // hit-testing each glyph's sub-rect. Indicators marked `clickable` are the exception and
    // carry their own tooltip.
    std::string tooltip;
    // Optional line under the name, e.g. a hard drive's "live" / "pending".
    std::string subtitle;
    ImVec4 subtitleColor = ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
};

// Draws the strip and returns the index the user just clicked, or -1 if the selection did not
// change. `selected` is the currently highlighted chip. When `outClick` is given it receives the
// clickable indicator the user pressed, if any; a press on an indicator never counts as a
// selection change.
int DrawDriveChips(const char* id, const std::vector<DriveChip>& chips, int selected,
                   ChipIndicatorClick* outClick = nullptr);

// Two-digit seven-segment readout drawn to `dl`. Unlit segments are drawn in `off` rather than
// skipped, so the "88" ghost shows the way a real LED display does - and so the readout occupies
// the same width at every value. `value` may be kNoDigits for "--".
void DrawSevenSegment(ImDrawList* dl, ImVec2 pos, float height, int value, ImU32 on, ImU32 off);

// Width the readout occupies for a given height, so callers can reserve space for it.
float SevenSegmentWidth(float height);

} // namespace ui
} // namespace adm
