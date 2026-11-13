#include "BlitOverlayController.h"

#include "../core/AmigaScreenMap.h"
#include "../core/BitplaneLayout.h"
#include "../core/BlitProfiler.h"
#include "../core/UserPreferences.h"
#include "views/BlitterView.h"
#include "views/DebuggerView.h"
#include "views/EmulatorView.h"

namespace adm::ui {

void BlitOverlayController::Init(EmulatorView* emulatorView, DebuggerView* debuggerView,
                                 BlitterView* blitterView, core::BlitProfiler* blitProfiler,
                                 core::UserPreferences* userPrefs, BlitOverlayOps ops)
{
    m_emulatorView = emulatorView;
    m_debuggerView = debuggerView;
    m_blitterView = blitterView;
    m_blitProfiler = blitProfiler;
    m_userPrefs = userPrefs;
    m_ops = std::move(ops);
}

// Resolve the scrubber's active blit (hovered/pinned) to this frame's record. The pointer is
// into the profiler's current-frame vector, which is stable for the rest of the draw pass.
const core::BlitRecord* BlitOverlayController::ResolveActiveBlit() const
{
    if (m_activeBlitLRef == UINT32_MAX || !m_blitProfiler)
        return nullptr;
    for (const auto& r : m_blitProfiler->GetCurrentFrame()) {
        if (r.lRef == m_activeBlitLRef)
            return &r;
    }
    return nullptr;
}

void BlitOverlayController::SetSettings(const BlitOverlaySettings& settings)
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;

    const bool wantProvenance = settings.enabled && settings.useProvenance;
    if (emu)
        emu->SetBlitterVisEnabled(wantProvenance);
    if (!wantProvenance)
        m_blitVisOverlay.Reset(); // don't hold a frame-sized RGBA buffer while switched off

    m_blitVisOverlay.SetOpacity(settings.opacity);
    m_blitVisOverlay.SetDecayFrames(settings.decayFrames);
}

void BlitOverlayController::SetDestCaptureEnabled(bool enabled)
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (emu)
        emu->SetBlitDataCaptureEnabled(enabled);
    SyncDestCapture();
}

// Keeps the core's capture arming, the Blitter window's checkbox and the saved preference in
// agreement. Runs every frame because the core is loaded long after preferences are restored,
// and a core reload drops the arming without telling anyone.
void BlitOverlayController::SyncDestCapture()
{
    if (!m_blitterView || !m_debuggerView)
        return;
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    const bool supported = emu && emu->HasBlitDataCapture();

    // Two mirrors: the Display panel owns the control, the Blitter window still needs the state
    // to decide whether to show (and make room for) its Data column.
    m_debuggerView->SetBlitCaptureDestDataSupported(supported);
    m_blitterView->SetCaptureDestDataSupported(supported);
    if (!supported) {
        m_debuggerView->SetBlitCaptureDestData(false);
        m_blitterView->SetCaptureDestData(false);
        return;
    }

    const bool want = m_userPrefs->Get().captureBlitDestData;
    if (emu->IsBlitDataCaptureEnabled() != want)
        emu->SetBlitDataCaptureEnabled(want);
    const bool on = emu->IsBlitDataCaptureEnabled();
    m_debuggerView->SetBlitCaptureDestData(on);
    m_blitterView->SetCaptureDestData(on);
}

// Keeps the core's provenance arming in agreement with the overlay's settings.
//
// The same reason SyncBlitDestCapture exists: retro_load_game resets the core's debug toggles
// without telling anyone, so an overlay left switched on goes quietly dead after a disk swap and
// the panel keeps claiming it is showing something. One bool compare a frame buys that back.
void BlitOverlayController::SyncProvenance()
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu || !m_debuggerView)
        return;

    const auto& s = m_debuggerView->GetBlitOverlaySettings();
    const bool want = s.enabled && s.useProvenance;
    if (emu->IsBlitterVisEnabled() != want)
        emu->SetBlitterVisEnabled(want);
}

// The blit overlay: one feature, two renderers.
//
// The provenance fill goes down first and covers everything the display actually fetched -- it is
// the ground truth of what the blitter put on screen. A blit whose destination is never scanned
// out produces no spans at all, though, and that is not a failure of the technique but inherent
// to it. Those blits, and by default only those, get a rectangle rebuilt from bltdpt/bltdmod
// geometry drawn on top instead (see core/BlitScreenMapper.h). So every blit in the frame is
// accounted for, and the user never has to work out which mechanism can answer their question.
void BlitOverlayController::Draw(ImDrawList* dl, ImVec2 imgMin, ImVec2 imgMax)
{
    if (!m_emulatorView->IsRunning())
        return;

    auto* emu = m_emulatorView->GetEmulator();
    if (!emu)
        return;

    const BlitOverlaySettings s = m_debuggerView->GetBlitOverlaySettings();
    if (!s.enabled) {
        // Release the scrubber's highlight if the pick still owns it, or a stale highlight would
        // outlive the overlay that set it.
        if (m_provenancePickLRef != UINT32_MAX) {
            m_provenancePickLRef = UINT32_MAX;
            m_debuggerView->SetHighlightedBlit(UINT32_MAX);
        }
        m_debuggerView->SetBlitOverlayStats(0, 0, 0, 0);
        return;
    }

    const bool provenanceOn = s.useProvenance && emu->IsBlitterVisEnabled();

    // --- Layer 1: the provenance fill ---------------------------------------------------------
    if (provenanceOn) {
        // Isolate the pinned/selected blit so its spans stay bright while the rest dim.
        uint32_t isolate = 0;
        if (m_activeBlitLRef != UINT32_MAX) {
            for (const auto& r : m_blitProfiler->GetCurrentFrame()) {
                if (r.lRef == m_activeBlitLRef) {
                    isolate = r.blitId;
                    break;
                }
            }
        }
        m_blitVisOverlay.SetIsolatedBlitId(isolate);
        m_blitVisOverlay.Draw(dl, imgMin, imgMax);
    }

    // --- Layer 2: geometry boxes --------------------------------------------------------------
    core::BitplaneLayout layout = m_debuggerView->GetBitplaneLayout();
    core::AmigaScreenMap map =
        core::AmigaScreenMap::Build(emu->GetDisplayGeometry(), static_cast<int>(emu->GetWidth()),
                                    static_cast<int>(emu->GetHeight()));

    const auto& records = m_blitProfiler->GetCurrentFrame();

    // Boxes for the blits the fill could not show, kept alongside their records so the pick can
    // hit-test them without recomputing the geometry.
    m_blitBoxes.clear();
    m_blitBoxRecords.clear();
    int pixelBlits = 0;
    // Blits neither renderer can place: no tagged pixels, and a destination the geometry mapper
    // cannot resolve to a bitplane buffer (line-mode blits, and copies into memory that is not a
    // display buffer at all). Counting them is what makes the stats line a complete account --
    // without it the panel silently under-reports and "the counts add up" stops being a usable
    // check that the fallback detector is right.
    int unplacedBlits = 0;

    for (const auto& r : records) {
        // A blit the fill already covers needs no box unless the user asked to outline
        // everything. With provenance off nothing is covered, so everything gets one.
        const bool shown = provenanceOn && m_blitVisOverlay.HasSpansThisFrame(r.blitId);
        if (shown)
            ++pixelBlits;
        if (shown && !s.outlineAll)
            continue;

        core::BlitOverlayBox box = core::MapBlitDestToScreen(r, layout, map);
        if (!box.valid) {
            // Only a blit the fill did not show is missing from the overlay; an outlineAll pass
            // over an already-filled blit that fails to map has lost nothing.
            if (!shown)
                ++unplacedBlits;
            continue;
        }
        m_blitBoxes.push_back(box);
        m_blitBoxRecords.push_back(&r);
    }

    m_debuggerView->SetBlitOverlayStats(m_blitVisOverlay.GetLastSpanCount(), pixelBlits,
                                        static_cast<int>(m_blitBoxes.size()), unplacedBlits);

    auto toScreen = [&](float u, float v) -> ImVec2 {
        return {imgMin.x + u * (imgMax.x - imgMin.x), imgMin.y + v * (imgMax.y - imgMin.y)};
    };

    // Draws one blit destination box. `emphasize` is the active/pinned blit (thicker, filled,
    // labelled); the bulk boxes are thin outlines so overlap stays readable.
    auto drawBox = [&](const core::BlitOverlayBox& box, bool emphasize) {
        ImVec2 tl = toScreen(box.u0, box.v0);
        ImVec2 br = toScreen(box.u1, box.v1);

        // Green = on-screen (front) buffer; yellow = off-screen (back) buffer of same geometry.
        const bool front = (box.kind == core::BlitBufferKind::Front);
        if (emphasize) {
            const ImU32 col = front ? IM_COL32(90, 255, 120, 230) : IM_COL32(255, 220, 70, 230);
            dl->AddRectFilled(tl, br,
                              front ? IM_COL32(90, 255, 120, 40) : IM_COL32(255, 220, 70, 40));
            dl->AddRect(tl, br, col, 0.0f, 0, 2.0f);
            dl->AddText({tl.x, br.y + 2.0f}, col, front ? "blit dest" : "blit dest (back buffer)");
        } else {
            const ImU32 col = front ? IM_COL32(90, 255, 120, 150) : IM_COL32(255, 220, 70, 150);
            dl->AddRect(tl, br, col, 0.0f, 0, 1.0f);
        }
    };

    // The active/pinned blit is drawn last so it lands on top of the bulk boxes.
    int activeIdx = -1;
    for (size_t i = 0; i < m_blitBoxes.size(); ++i) {
        if (m_blitBoxRecords[i]->lRef == m_activeBlitLRef) {
            activeIdx = static_cast<int>(i);
            continue;
        }
        drawBox(m_blitBoxes[i], false);
    }
    if (activeIdx >= 0)
        drawBox(m_blitBoxes[static_cast<size_t>(activeIdx)], true);

    PickBlit(imgMin, imgMax, m_blitBoxes, m_blitBoxRecords);
}

// The overlay read backwards: a point on the picture resolves to the blit that wrote it.
//
// Two surfaces answer, in order of trustworthiness. The provenance id map is exact -- every pixel
// literally carries the id of the blit it was fetched from, so there is no geometry and no
// guessing which plane or buffer it came from. Only when that says nothing do the geometry boxes
// get a turn, which is what makes a blit into an off-screen buffer clickable at all.
//
// Hovering highlights that blit's bar in the frame scrubber; Alt+click pins its detail panel and
// syncs the other time-aware windows to it. Alt is the modifier because a plain left click over
// the picture grabs the Amiga mouse; the capture blocker (see SetMouseCaptureBlocker below)
// stands the capture down while Alt is held and the overlay is on.
void BlitOverlayController::PickBlit(ImVec2 imgMin, ImVec2 imgMax,
                                     const std::vector<core::BlitOverlayBox>& boxes,
                                     const std::vector<const core::BlitRecord*>& boxRecords)
{
    const float spanX = imgMax.x - imgMin.x;
    const float spanY = imgMax.y - imgMin.y;

    uint32_t lRef = UINT32_MAX;
    const core::BlitRecord* hit = nullptr;
    bool exact = false;

    const bool overPicture = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                             ImGui::IsMouseHoveringRect(imgMin, imgMax) && spanX > 0.0f &&
                             spanY > 0.0f;

    if (overPicture) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float u = (mouse.x - imgMin.x) / spanX;
        const float v = (mouse.y - imgMin.y) / spanY;

        const int texW = m_blitVisOverlay.GetTexWidth();
        const int texH = m_blitVisOverlay.GetTexHeight();
        if (texW > 0 && texH > 0) {
            const int px = static_cast<int>(u * static_cast<float>(texW));
            const int py = static_cast<int>(v * static_cast<float>(texH));

            // A pixel still fading from an earlier frame keeps its id, but the profiler only
            // holds the current frame's records — so an old id simply finds no match and picks
            // nothing, falling through to the boxes below.
            const uint32_t blitId = m_blitVisOverlay.BlitIdAt(px, py);
            if (blitId != 0) {
                for (const auto& r : m_blitProfiler->GetCurrentFrame()) {
                    if (r.blitId == blitId) {
                        hit = &r;
                        lRef = r.lRef;
                        exact = true;
                        break;
                    }
                }
            }
        }

        if (!hit && !boxes.empty()) {
            const int idx =
                core::PickSmallestBoxContaining(boxes.data(), static_cast<int>(boxes.size()), u, v);
            if (idx >= 0 && static_cast<size_t>(idx) < boxRecords.size()) {
                hit = boxRecords[static_cast<size_t>(idx)];
                lRef = hit->lRef;
            }
        }
    }

    if (hit) {
        ImGui::BeginTooltip();
        ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.65f, 1.0f), "Blit #%u", hit->blitId);
        ImGui::Separator();
        ImGui::Text("L-Ref:  %u  (line %u, clock %u)", hit->lRef, hit->startLine, hit->startClock);
        ImGui::Text("Size:   %u x %u words", hit->rows, hit->wordsPerRow);
        ImGui::Text("Dest:   $%08X", hit->raw.bltdpt);
        ImGui::Text("Source: %s%s", hit->raw.fromCopper ? "Copper" : "CPU",
                    hit->lineMode ? " (line draw)" : "");
        ImGui::Text("Minterm: $%02X", hit->minterm);

        // Which surface answered, because they are not equally trustworthy: the pixels are what
        // the display fetched, the box is where the geometry says the write landed.
        ImGui::Separator();
        if (exact)
            ImGui::TextDisabled("Tagged pixels \xc2\xb7 exact");
        else
            ImGui::TextDisabled("Geometry box \xc2\xb7 destination not scanned out");
        ImGui::TextDisabled("Alt+click to pin this blit in the Frame Scrubber");
        ImGui::EndTooltip();
    }

    // Only touch the scrubber's highlight channel while the pick owns it: writing every frame
    // would stomp a highlight coming from a Blitter Operations row hover.
    if (lRef != m_provenancePickLRef) {
        m_provenancePickLRef = lRef;
        m_debuggerView->SetHighlightedBlit(lRef);
    }

    if (hit && ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_debuggerView->PinBlit(lRef);
        m_debuggerView->JumpToLRef(lRef);
        if (m_ops.syncMintermToBlit)
            m_ops.syncMintermToBlit(lRef);
    }
}

} // namespace adm::ui
