#pragma once

namespace adm::ui {

// User-facing settings for the blit overlay drawn on the emulator picture.
//
// One feature, two renderers. The core tags pixels as the display fetches them, which is exact
// but can only ever describe pixels that were actually scanned out; a blit into a buffer that is
// never displayed produces nothing. For those, and only those, a rectangle reconstructed from
// bltdpt/bltdmod geometry is drawn instead (see core/BlitScreenMapper.h). The user does not pick
// between the two -- each blit gets whichever renderer can locate it.
//
// Lives in its own header because ownership is split, exactly as RasterOverlaySettings does: the
// controls are on DebuggerDisplayView, but Application owns the overlay itself (it owns the
// emulator the spans come from) and DebuggerView passes the settings between them.
struct BlitOverlaySettings
{
    bool enabled = false; // master: draw the blit overlay on the emulator picture

    // Arm the core-side provenance map. Off leaves geometry boxes for every blit, which costs
    // nothing -- the map is roughly 50 MB of tables plus a per-fetch lookup while it is on.
    // Forced off on a core that predates the patch, where the boxes are all there is.
    bool useProvenance = true;

    // Additionally outline on-screen destinations, which the provenance fill already covers.
    // Off by default: the fill says the same thing more precisely, and a box per blit buries
    // the picture on a frame with a few hundred of them.
    bool outlineAll = false;

    float opacity = 0.65f; // provenance fill opacity
    int decayFrames = 5;   // frames a span stays visible; 1 makes a one-shot blit a flicker
};

} // namespace adm::ui
