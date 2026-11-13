#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <imgui.h>

#include "../core/BlitScreenMapper.h"
#include "BlitOverlaySettings.h"
#include "views/BlitterVisOverlay.h"

namespace adm::core {
class BlitProfiler;
class UserPreferences;
struct BlitRecord;
} // namespace adm::core

namespace adm::ui {

class BlitterView;
class DebuggerView;
class EmulatorView;

/**
 * @brief The one thing the overlay cannot do for itself.
 *
 * Pinning a blit also pushes it at the Blitter Minterm window, which is resolved through the
 * profiler and that window - neither of which this overlay otherwise touches.
 */
struct BlitOverlayOps
{
    std::function<void(uint32_t lRef)> syncMintermToBlit;
};

/**
 * @brief Draws the blit-destination overlay on the emulator display, and picks blits back out
 *        of it.
 *
 * One feature, two renderers - see the comment above Draw() in the .cpp for why. Owns the
 * provenance overlay, the geometry boxes rebuilt each frame, and the two highlight references
 * that decide what is emphasised and what the scrubber is told.
 *
 * Extracted from Application.
 */
class BlitOverlayController
{
public:
    /// All four are owned by Application and outlive this object; any may be null.
    void Init(EmulatorView* emulatorView, DebuggerView* debuggerView, BlitterView* blitterView,
              core::BlitProfiler* blitProfiler, core::UserPreferences* userPrefs,
              BlitOverlayOps ops);

    // Applies the debugger panel's settings to the core (allocating/freeing its ~50 MB of
    // tables), to the overlay's texture, and to its opacity/decay.
    void SetSettings(const BlitOverlaySettings& settings);

    // Arms/disarms the core's capture of the words each blit writes, which is what the
    // Blitter window's Data column and the scrubber's blit tooltip render.
    void SetDestCaptureEnabled(bool enabled);

    // Reconciles that arming with the checkbox and the saved preference; called each frame.
    void SyncDestCapture();

    // Same, for the overlay's provenance map: a core reload drops the arming silently.
    void SyncProvenance();

    /// Repaint the provenance overlay from this frame's spans.
    void UpdateProvenance(AmigaEmulator& emu) { m_blitVisOverlay.Update(emu); }

    void Draw(ImDrawList* dl, ImVec2 imgMin, ImVec2 imgMax);

    /// Which blit the scrubber has hovered or pinned (UINT32_MAX = none).
    void SetActiveBlitLRef(uint32_t lRef) { m_activeBlitLRef = lRef; }

    /// The active blit's record in this frame, or null. The pointer is into the profiler's
    /// current-frame vector, which is stable for the rest of the draw pass.
    const core::BlitRecord* ResolveActiveBlit() const;

private:
    // Reverse of the overlay: turns the point under the cursor back into the blit that wrote it,
    // highlighting it in the scrubber on hover and pinning it on Alt+click. Tries the provenance
    // id map first (exact), then the geometry boxes Draw just laid down.
    void PickBlit(ImVec2 imgMin, ImVec2 imgMax, const std::vector<core::BlitOverlayBox>& boxes,
                  const std::vector<const core::BlitRecord*>& boxRecords);

    EmulatorView* m_emulatorView = nullptr;
    DebuggerView* m_debuggerView = nullptr;
    BlitterView* m_blitterView = nullptr;
    core::BlitProfiler* m_blitProfiler = nullptr;
    core::UserPreferences* m_userPrefs = nullptr;
    BlitOverlayOps m_ops;

    // Blit-destination overlay: the scrubber reports which blit to highlight (UINT32_MAX =
    // none); each frame this is drawn on the emulator display showing where the destination
    // lands on screen.
    uint32_t m_activeBlitLRef = UINT32_MAX;

    // Fetch-time blit provenance overlay. The primary renderer: the core tags pixels as the
    // display fetches them, so no coordinate reconstruction is involved. Geometry boxes fill in
    // automatically for the blits it cannot see -- see Draw.
    BlitterVisOverlay m_blitVisOverlay;

    // The geometry boxes drawn this frame and the records they belong to, kept as members purely
    // so the per-frame rebuild does not reallocate. Valid only for the duration of one
    // Draw call; the record pointers are into the profiler's current-frame vector.
    std::vector<core::BlitOverlayBox> m_blitBoxes;
    std::vector<const core::BlitRecord*> m_blitBoxRecords;

    // Blit last pushed into the scrubber's highlight channel by the pick (UINT32_MAX = none).
    // Tracked so the pick only writes that shared channel while it actually owns it, instead
    // of clearing a highlight the Blitter Operations window set.
    uint32_t m_provenancePickLRef = UINT32_MAX;
};

} // namespace adm::ui
