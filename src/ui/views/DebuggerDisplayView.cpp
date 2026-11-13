#include "DebuggerDisplayView.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include "../../core/AmigaCustom.h"
#include "../../core/AmigaEmulator.h"
#include "../../core/AmigaScreenMap.h"
#include "../../core/BitplaneDecoder.h"
#include "../../core/BitplaneLayout.h"
#include "../../core/BlitVisRaster.h"
#include "../../core/Constants.h"
#include "../../core/CopperRaster.h"
#include "../DmaColors.h"
#include "../IconsFontAwesome6.h"
#include "../Magnifier.h"
#include "EmulatorView.h"

namespace adm::ui {

namespace {

namespace beam = adm::core::constants::beam;

// A Denise lores position and the beam position that drew it. Denise trails the beam by
// beam::DeniseLagCCK clocks, and a displayed line runs on past the end of its Agnus line, so the
// right-hand tail of line V was drawn during line V + 1. The raster overlay lays the frame out
// the same way, so a clicked cell and the crosshair it moves agree.
struct BeamPos
{
    int line;
    int cck;
};

BeamPos BeamFromDenise(int hpix, int vscan)
{
    const int lineCC = static_cast<int>(beam::CCKPerAgnusLine);
    BeamPos b{vscan, hpix / 2 + static_cast<int>(beam::DeniseLagCCK)};
    if (b.cck >= lineCC) {
        b.cck -= lineCC;
        ++b.line;
    }
    return b;
}

uint32_t LRefFromDenise(int hpix, int vscan)
{
    const BeamPos b = BeamFromDenise(hpix, vscan);
    return static_cast<uint32_t>(std::max(0, b.line)) * beam::CCKPerLine +
           static_cast<uint32_t>(std::max(0, b.cck));
}

} // namespace

DebuggerDisplayView::DebuggerDisplayView(EmulatorView* emulatorView) : m_emulatorView(emulatorView)
{
}

void DebuggerDisplayView::SetCrosshairLRef(uint32_t lRef)
{
    // Convert L-Ref back to the lores-pixel/scanline coordinates amigaToScreen uses: the pixel
    // the beam was drawing at that clock. The first few clocks of a line draw the tail of the
    // line above (see BeamFromDenise).
    int line = static_cast<int>(lRef / beam::CCKPerLine);
    int hpix = (static_cast<int>(lRef % beam::CCKPerLine) - static_cast<int>(beam::DeniseLagCCK)) *
               2; // CCK → lores pixel
    if (hpix < 0 && line > 0) {
        --line;
        hpix += static_cast<int>(beam::CCKPerAgnusLine) * 2;
    }
    m_lockedVscan = line;
    m_lockedHpix = hpix;
    m_crosshairLocked = true;
}

void DebuggerDisplayView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void DebuggerDisplayView::Draw()
{
    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }

    if (ImGui::Begin(ICON_FA_DESKTOP " Display###Display##debugger", &m_open, flags)) {
        DrawContent();
    }
    ImGui::End();
}

void DebuggerDisplayView::PushDisplayMasks()
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu)
        return;

    // The emulator owns the masks (shared with the Sprites window, the AR console and MCP) and
    // re-renders the frame itself, so the change shows up immediately even while paused.
    emu->SetDisplayMasks({m_bitplaneMask, m_spriteMask, m_playfieldMask});
}

bool DebuggerDisplayView::BeginSection(const char* label)
{
    auto it = m_sections.find(label); // heterogeneous lookup — no temporary key string
    const bool wasOpen = (it == m_sections.end()) ? true : it->second;
    // ImGuiCond_Always every frame: the workspace owns this state, not ImGui's ini.
    ImGui::SetNextItemOpen(wasOpen, ImGuiCond_Always);
    const bool nowOpen = ImGui::CollapsingHeader(label);
    if (it != m_sections.end())
        it->second = nowOpen;
    else
        m_sections.emplace(label, nowOpen); // first frame this header appears
    return nowOpen;
}

bool DebuggerDisplayView::BeginTwoColumns(const char* id)
{
    return ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame);
}

void DebuggerDisplayView::DrawContent()
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (emu) {
        m_videoResolution = emu->GetVideoResolution();
        // Refresh from the shared state so toggles made in the Sprites window, the AR console
        // (BPL/SPR/PF) or via MCP are reflected by these checkboxes.
        const auto masks = emu->GetDisplayMasks();
        m_bitplaneMask = masks.bitplanes;
        m_spriteMask = masks.sprites;
        m_playfieldMask = masks.playfields;
        m_paletteVis = emu->GetPaletteVisEnabled();
        m_bplcon1Scroll = emu->GetBplcon1ScrollEnabled();
        m_copperBlock = emu->GetCopperBlock();
        m_bplPtrBlock = emu->GetBplPtrBlock();
        m_blitterWrites = emu->GetBlitterWritesEnabled();
    }

    // Declared ahead of the sections: the "Show All" button at the bottom needs them even when
    // the section that would have set them is collapsed.
    bool bplChanged = false;
    bool sprChanged = false;
    bool pfChanged = false;

    adm::core::Custom customRegs{};
    bool hasRegs = m_emulatorView && m_emulatorView->IsRunning() &&
                   m_emulatorView->GetEmulator()->GetCustomRegisters(customRegs);

    if (BeginSection("Overlays")) {
        if (BeginTwoColumns("##overlaytable")) {
            ImGui::TableSetupColumn("##ovl_left", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##ovl_right", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("Performance##perf", m_emulatorView->GetShowPerfOverlayPtr());

            ImGui::TableSetColumnIndex(1);
            ImGui::Checkbox("Frame Counter##fc", &m_showFrameCounterOverlay);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("Geometry##geo", &m_showGeometryOverlay);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Draw DIW (display window) and DDF (data fetch) boundaries on the emulator "
                    "screen, "
                    "with a click-to-lock crosshair that reports Denise/Agnus/L-Ref coordinates");

            ImGui::TableSetColumnIndex(1);
            {
                // Three-way, not a checkbox: an AGA super-hires screen rendered into a hires
                // framebuffer is downsampled before the magnifier or any overlay can see it.
                static const char* kResNames[] = {"Lores", "Hires", "Super Hires"};
                int resIdx = static_cast<int>(m_videoResolution);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##videores", &resIdx, kResNames, IM_ARRAYSIZE(kResNames))) {
                    m_videoResolution = static_cast<adm::AmigaEmulator::VideoResolution>(resIdx);
                    if (emu)
                        emu->SetVideoResolution(m_videoResolution);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Internal render resolution. Anything above Lores also enables vertical "
                        "doubling and progressive interlace handling. Use Super Hires to inspect "
                        "AGA "
                        "super-hires screens pixel-for-pixel.");
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Checkbox("Sprite BBoxes##sbb", &m_showSpriteBBoxes)) {
                if (m_onShowSpriteBBoxesChanged)
                    m_onShowSpriteBBoxesChanged(m_showSpriteBBoxes);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Show bounding box overlays for all sprites on the emulator screen");

            ImGui::TableSetColumnIndex(1);
            // Magnifier shares its flag with the Amiga Emulator toolbar magnify button.
            ImGui::Checkbox("Magnifier##mag", m_emulatorView->GetShowMagnifierPtr());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Hover the emulator screen to zoom in on individual pixels (also "
                                  "toggled from the "
                                  "magnify button in the Amiga Emulator toolbar)");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            {
                // Not an overlay in the ImDrawList sense: the core replaces the pixel data, so
                // this one lives or dies with the DLL and is greyed out on an older core.
                const bool supported = emu && emu->HasPaletteVis();
                ImGui::BeginDisabled(!supported);
                if (ImGui::Checkbox("Palette Vis##palvis", &m_paletteVis) && emu)
                    emu->SetPaletteVisEnabled(m_paletteVis);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (supported)
                        ImGui::SetTooltip(
                            "Replace the picture with this frame's palette: every scanline becomes "
                            "one bar per pen (2^BPU of them), drawn in that line's colours.\n\n"
                            "Copper colour splits, fades and raster bars show up as changes down "
                            "the screen. Sprites still draw on top; the program's own graphics are "
                            "hidden while this is on.");
                    else
                        ImGui::SetTooltip("Not available in the loaded emulator core -- rebuild it "
                                          "with scripts/build_puae to enable this.");
                }
            }

            ImGui::TableSetColumnIndex(1);
            {
                // The same flag the Blitter section's "Blit Overlay" checkbox drives -- both read
                // and write m_blit.enabled, so the two stay in step with no extra state. Listed
                // here because this is where you look for "what is drawn over the picture"; the
                // Blitter section keeps the settings that shape it.
                if (ImGui::Checkbox("Blit Overlay##blitovlist", &m_blit.enabled) &&
                    m_onBlitOverlayChanged)
                    m_onBlitOverlayChanged(m_blit);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Colour every pixel on screen by the blit that last wrote it, and box the "
                        "destinations that never made it to screen.\n\nOpacity, the provenance "
                        "switch and the rest of its settings are in the Blitter section below.");
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            {
                // Shares m_raster.enabled with the Raster section's own checkbox, the same way
                // Blit Overlay shares m_blit.enabled -- one flag, two places to reach it.
                if (ImGui::Checkbox("Raster Overlay##rasterovlist", &m_raster.enabled) &&
                    m_onShowCopperRasterChanged)
                    m_onShowCopperRasterChanged(m_raster.enabled);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Inset the emulator picture inside the full 227x313 beam and colour every "
                        "colour clock by what the bus did in it.\n\nThe layers, DMA channels and "
                        "opacity are in the Raster (Beam View) section below.");
            }

            ImGui::EndTable();
        }
    }

    int hwBpuCount = 0;
    bool hwBplDmaOn = false;
    uint8_t hwSprActive = 0;
    if (hasRegs) {
        hwBpuCount = (customRegs.bplcon0 >> 12) & 0x7;
        if (customRegs.bplcon0 & 0x0010)
            hwBpuCount |= 0x8;
        if (hwBpuCount > 8)
            hwBpuCount = 8;
        hwBplDmaOn = (customRegs.dmaconr & (1 << 8)) != 0;

        bool sprDmaOn = (customRegs.dmaconr & (1 << 5)) != 0;
        if (sprDmaOn) {
            for (int i = 0; i < 8; i++) {
                if (customRegs.spr[i].pos != 0 || customRegs.spr[i].ctl != 0)
                    hwSprActive |= (1 << i);
            }
        }
    }

    const float ledRadius = 6.0f;
    const float ledDiameter = ledRadius * 2.0f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();

    auto DrawLed = [&](bool active, const char* invisId) {
        float itemMidY = ImGui::GetItemRectMin().y + ImGui::GetTextLineHeight() * 0.5f;
        float cursorX = ImGui::GetCursorPosX();
        ImVec2 center(winPos.x + cursorX + ledRadius, itemMidY);
        ImU32 fillColor = active ? IM_COL32(0, 255, 0, 255) : IM_COL32(0, 60, 0, 255);
        drawList->AddCircleFilled(center, ledRadius, fillColor);
        drawList->AddCircle(center, ledRadius, IM_COL32(80, 80, 80, 255), 0, 1.0f);
        ImVec2 btnMin(center.x - ledRadius - 2, center.y - ledRadius - 2);
        ImGui::SetCursorScreenPos(btnMin);
        ImGui::InvisibleButton(invisId, ImVec2(ledDiameter + 4, ledDiameter + 4));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(active ? "Active in hardware registers"
                                     : "Not active in hardware registers");
        }
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
        ImGui::SetCursorPosX(cursorX + ledDiameter + ImGui::GetStyle().ItemSpacing.x);
        ImGui::Dummy({0, 0});
    };

    // ---- Bitplanes -------------------------------------------------------
    if (BeginSection("Bitplanes")) {
        if (hasRegs) {
            if (hwBplDmaOn)
                ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.35f, 1.0f), "BPLEN on  BPU=%d",
                                   hwBpuCount);
            else
                ImGui::TextDisabled("BPLEN off  BPU=%d", hwBpuCount);

            // The screen mode belongs beside BPLEN/BPU: it is the same hardware state described two
            // ways, and it read as an unrelated section on its own. Same decode the geometry
            // overlay, the MCP tools and the AR console DISPLAY command use, so all four agree.
            ImGui::SameLine(0.0f, 16.0f);
            const core::ScreenMode mode = core::DescribeScreenMode(customRegs);
            ImGui::TextUnformatted(core::ScreenModeSummary(mode).c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Decoded from BPLCON0/DIW/DDF, in the pixels the program draws.\n"
                                  "DDF fetch: %dpx per row\n"
                                  "(DIW and DDF registers are in lores units at every resolution.)",
                                  mode.fetchWidth);
            }
        }

        ImGui::Spacing();

        {
            // Above the per-plane checkboxes because it is the other half of the same question:
            // which planes are drawn, and where the shifter puts them.
            const bool supported = emu && emu->HasBplcon1Scroll();
            ImGui::BeginDisabled(!supported);
            if (ImGui::Checkbox("BPLCON1 Scroll##bplcon1", &m_bplcon1Scroll) && emu)
                emu->SetBplcon1ScrollEnabled(m_bplcon1Scroll);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (supported)
                    ImGui::SetTooltip(
                        "On (default) = hardware behaviour. Off forces the BPLCON1 delay and "
                        "sub-pixel scroll fields to zero, so the bitplane shifter stops applying "
                        "fine horizontal scroll and the raw fetch alignment becomes visible -- "
                        "which also makes per-playfield scrolling obvious.\n\n"
                        "Applied during bitplane fetch, so it takes effect on the next emulated "
                        "frame rather than while paused.");
                else
                    ImGui::SetTooltip("Not available in the loaded emulator core -- rebuild it "
                                      "with scripts/build_puae to enable this.");
            }
        }

        ImGui::Spacing();

        {
            bool allOn = (m_bitplaneMask == 0xFF);
            bool mixed = (m_bitplaneMask != 0x00 && m_bitplaneMask != 0xFF);
            if (mixed)
                ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            if (ImGui::Checkbox("All Bitplanes##bplall", &allOn)) {
                m_bitplaneMask = allOn ? 0xFF : 0x00;
                bplChanged = true;
            }
            if (mixed)
                ImGui::PopItemFlag();
        }

        ImGui::Spacing();

        if (BeginTwoColumns("##bpltable")) {
            ImGui::TableSetupColumn("##bpl_left", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##bpl_right", ImGuiTableColumnFlags_WidthStretch);

            for (int i = 0; i < 4; i++) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                {
                    bool v = (m_bitplaneMask & (1 << i)) != 0;
                    char label[16];
                    snprintf(label, sizeof(label), "BPL%d##bpl%d", i + 1, i);
                    if (ImGui::Checkbox(label, &v)) {
                        if (v)
                            m_bitplaneMask |= (1 << i);
                        else
                            m_bitplaneMask &= ~(1 << i);
                        bplChanged = true;
                    }
                    bool hwOn = hasRegs && hwBplDmaOn && (i + 1 <= hwBpuCount);
                    char ledId[32];
                    snprintf(ledId, sizeof(ledId), "##bplledi%d", i);
                    ImGui::SameLine();
                    DrawLed(hwOn, ledId);
                }

                ImGui::TableSetColumnIndex(1);
                {
                    int j = i + 4;
                    bool v = (m_bitplaneMask & (1 << j)) != 0;
                    char label[16];
                    snprintf(label, sizeof(label), "BPL%d##bpl%d", j + 1, j);
                    if (ImGui::Checkbox(label, &v)) {
                        if (v)
                            m_bitplaneMask |= (1 << j);
                        else
                            m_bitplaneMask &= ~(1 << j);
                        bplChanged = true;
                    }
                    bool hwOn = hasRegs && hwBplDmaOn && (j + 1 <= hwBpuCount);
                    char ledId[32];
                    snprintf(ledId, sizeof(ledId), "##bplledi%d", j);
                    ImGui::SameLine();
                    DrawLed(hwOn, ledId);
                }
            }

            ImGui::EndTable();
        }
    }

    // ---- Sprites ---------------------------------------------------------
    if (BeginSection("Sprites")) {
        if (hasRegs) {
            bool sprDmaOn = (customRegs.dmaconr & (1 << 5)) != 0;
            if (sprDmaOn)
                ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.35f, 1.0f), "SPREN on");
            else
                ImGui::TextDisabled("SPREN off");
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::TextDisabled("(dot = non-zero pos/ctl)");
            ImGui::Spacing();
        }

        {
            bool allOn = (m_spriteMask == 0xFF);
            bool mixed = (m_spriteMask != 0x00 && m_spriteMask != 0xFF);
            if (mixed)
                ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            if (ImGui::Checkbox("All Sprites##sprall", &allOn)) {
                m_spriteMask = allOn ? 0xFF : 0x00;
                sprChanged = true;
            }
            if (mixed)
                ImGui::PopItemFlag();
        }

        ImGui::Spacing();

        if (BeginTwoColumns("##sprtable")) {
            ImGui::TableSetupColumn("##spr_left", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##spr_right", ImGuiTableColumnFlags_WidthStretch);

            for (int i = 0; i < 4; i++) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                {
                    bool v = (m_spriteMask & (1 << i)) != 0;
                    char label[16];
                    snprintf(label, sizeof(label), "SPR%d##spr%d", i, i);
                    if (ImGui::Checkbox(label, &v)) {
                        if (v)
                            m_spriteMask |= (1 << i);
                        else
                            m_spriteMask &= ~(1 << i);
                        sprChanged = true;
                    }
                    bool hwOn = (hwSprActive & (1 << i)) != 0;
                    char ledId[32];
                    snprintf(ledId, sizeof(ledId), "##sprledi%d", i);
                    ImGui::SameLine();
                    DrawLed(hwOn, ledId);
                }

                ImGui::TableSetColumnIndex(1);
                {
                    int j = i + 4;
                    bool v = (m_spriteMask & (1 << j)) != 0;
                    char label[16];
                    snprintf(label, sizeof(label), "SPR%d##spr%d", j, j);
                    if (ImGui::Checkbox(label, &v)) {
                        if (v)
                            m_spriteMask |= (1 << j);
                        else
                            m_spriteMask &= ~(1 << j);
                        sprChanged = true;
                    }
                    bool hwOn = (hwSprActive & (1 << j)) != 0;
                    char ledId[32];
                    snprintf(ledId, sizeof(ledId), "##sprledi%d", j);
                    ImGui::SameLine();
                    DrawLed(hwOn, ledId);
                }
            }

            ImGui::EndTable();
        }
    }

    // ---- Playfields ------------------------------------------------------
    if (BeginSection("Playfields")) {
        bool hwDblpf = hasRegs && (customRegs.bplcon0 & (1 << 10)) != 0;
        if (hasRegs) {
            if (hwDblpf)
                ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.35f, 1.0f), "DBLPF on");
            else
                ImGui::TextDisabled("DBLPF off (single playfield)");
            ImGui::Spacing();
        }

        {
            bool allOn = (m_playfieldMask == 0x3);
            bool mixed = (m_playfieldMask != 0x0 && m_playfieldMask != 0x3);
            if (mixed)
                ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            if (ImGui::Checkbox("All Playfields##pfall", &allOn)) {
                m_playfieldMask = allOn ? 0x3 : 0x0;
                pfChanged = true;
            }
            if (mixed)
                ImGui::PopItemFlag();
        }

        ImGui::Spacing();

        if (BeginTwoColumns("##pftable")) {
            ImGui::TableSetupColumn("##pf_left", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##pf_right", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            {
                bool v = (m_playfieldMask & 1) != 0;
                if (ImGui::Checkbox("Playfield 1##pf1", &v)) {
                    if (v)
                        m_playfieldMask |= 1;
                    else
                        m_playfieldMask &= ~1u;
                    pfChanged = true;
                }
                bool hwOn = hasRegs && hwDblpf && hwBplDmaOn && hwBpuCount >= 1;
                ImGui::SameLine();
                DrawLed(hwOn, "##pfledi0");
            }

            ImGui::TableSetColumnIndex(1);
            {
                bool v = (m_playfieldMask & 2) != 0;
                if (ImGui::Checkbox("Playfield 2##pf2", &v)) {
                    if (v)
                        m_playfieldMask |= 2;
                    else
                        m_playfieldMask &= ~2u;
                    pfChanged = true;
                }
                bool hwOn = hasRegs && hwDblpf && hwBplDmaOn && hwBpuCount >= 2;
                ImGui::SameLine();
                DrawLed(hwOn, "##pfledi1");
            }

            ImGui::EndTable();
        }
    }

    DrawBlocksSection();

    DrawBlitterSection();

    DrawRasterSection();

    // ---- Master restore --------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Show All", ImVec2(-1.0f, 0.0f))) {
        m_bitplaneMask = 0xFF;
        m_spriteMask = 0xFF;
        m_playfieldMask = 0x3;
        bplChanged = true;
        sprChanged = true;
        pfChanged = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Restore all bitplanes, sprites and playfields");

    if (bplChanged || sprChanged || pfChanged)
        PushDisplayMasks();
}

// Copper and bitplane-pointer write blocks. Unlike the masks above these are NOT cosmetic: a
// suppressed write really does not happen, so the program's picture changes. What they do not
// change is timing -- the copper keeps running, WAIT/SKIP still execute and the cycles are still
// consumed -- which is what makes them a bisection tool rather than a perturbation.
//
// Adapted from the ENGINE9000 debugger by alpine9000 -- see PATCHES.md ("chipset-blocks").
void DebuggerDisplayView::DrawBlocksSection()
{
    if (!BeginSection("Copper & Bitplane Blocks"))
        return;

    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    const bool supported = emu && emu->HasChipsetBlocks();

    if (!supported) {
        ImGui::TextDisabled("Not available in the loaded emulator core");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rebuild the core with scripts/build_puae to enable these.");
        return;
    }

    // A line-range pair. Returns true when either end changed. Kept local because both blocks
    // want exactly the same widget and nothing else does.
    auto drawLineRange = [](const char* id, int& startLine, int& endLine, bool enabled) {
        bool changed = false;
        ImGui::BeginDisabled(!enabled);
        ImGui::TextUnformatted("Lines");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        char startId[32];
        snprintf(startId, sizeof(startId), "##%sstart", id);
        if (ImGui::InputInt(startId, &startLine, 0, 0)) {
            startLine = adm::core::ClampBlockLine(startLine);
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("to");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        char endId[32];
        snprintf(endId, sizeof(endId), "##%send", id);
        if (ImGui::InputInt(endId, &endLine, 0, 0)) {
            endLine = adm::core::ClampBlockLine(endLine);
            changed = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Inclusive scanline range (vpos). The default 52-308 spans roughly "
                              "the PAL visible display window.");
        return changed;
    };

    // ---- Copper block ----------------------------------------------------
    {
        bool changed = false;
        if (ImGui::Checkbox("Copper Block##copblk", &m_copperBlock.enabled))
            changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Drop every register write the Copper makes on these lines. The Copper still "
                "runs -- WAITs, SKIPs and its DMA cycles are unchanged -- so only its effect "
                "disappears, and CPU writes still land.\n\n"
                "Sweep the range down the screen: whatever stops changing inside it is what the "
                "Copper was driving there.");

        ImGui::Indent();
        if (drawLineRange("cop", m_copperBlock.startLine, m_copperBlock.endLine,
                          m_copperBlock.enabled))
            changed = true;
        ImGui::Unindent();

        if (changed)
            emu->SetCopperBlock(m_copperBlock);
    }

    ImGui::Spacing();

    // ---- Bitplane pointer block ------------------------------------------
    {
        bool changed = false;
        {
            const uint8_t all = adm::core::kBplPtrBlockMaskAll;
            bool allOn = (m_bplPtrBlock.mask == all);
            const bool mixed = (m_bplPtrBlock.mask != 0 && m_bplPtrBlock.mask != all);
            if (mixed)
                ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            if (ImGui::Checkbox("Bitplane Ptr Block##bplptrblk", &allOn)) {
                m_bplPtrBlock.mask = allOn ? all : 0;
                changed = true;
            }
            if (mixed)
                ImGui::PopItemFlag();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Drop Copper writes to the selected BPLxPT pointers on these lines, pinning them "
                "at whatever the last permitted write left. CPU writes still land.\n\n"
                "Mid-screen re-pointing is how split screens, hardware scrollers and "
                "Copper-driven buffer swaps work, so the part of the picture a pinned plane feeds "
                "visibly repeats -- which tells you which MOVE drives which plane, and where.");

        ImGui::Indent();
        if (drawLineRange("bpp", m_bplPtrBlock.startLine, m_bplPtrBlock.endLine, true))
            changed = true;

        // Planes 1-6: BPL7PT/BPL8PT are AGA-only and outside the hooked register range.
        if (BeginTwoColumns("##bplptrblocktable")) {
            for (int i = 0; i < adm::core::kBplPtrBlockPlaneCount; ++i) {
                if ((i % 2) == 0)
                    ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(i % 2);
                bool v = (m_bplPtrBlock.mask & (1u << i)) != 0;
                char label[24];
                snprintf(label, sizeof(label), "BPL%dPT##bpp%d", i + 1, i);
                if (ImGui::Checkbox(label, &v)) {
                    if (v)
                        m_bplPtrBlock.mask |= static_cast<uint8_t>(1u << i);
                    else
                        m_bplPtrBlock.mask &= static_cast<uint8_t>(~(1u << i));
                    changed = true;
                }
            }
            ImGui::EndTable();
        }
        ImGui::Unindent();

        if (changed)
            emu->SetBplPtrBlock(m_bplPtrBlock);
    }

    ImGui::Spacing();

    // ---- Blitter writes --------------------------------------------------
    // Not copper-scoped and not line-limited like the two above, but the same idea: suppress
    // the effect, keep the timing.
    {
        const bool blitterSupported = emu->HasBlitterWrites();
        ImGui::BeginDisabled(!blitterSupported);
        if (ImGui::Checkbox("Blitter Writes##blitwr", &m_blitterWrites))
            emu->SetBlitterWritesEnabled(m_blitterWrites);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (blitterSupported)
                ImGui::SetTooltip(
                    "On (default) = normal. Off stops the Blitter's destination (D channel) "
                    "writes reaching chip RAM, so nothing it draws lands -- while the blit "
                    "itself still runs: BLTSIZE still starts it, its DMA cycles are still "
                    "consumed, and BUSY and the completion interrupt still behave.\n\n"
                    "Whatever stops updating on screen is what the Blitter was drawing. Covers "
                    "every blit path -- normal, descending, fill, line and cycle-exact.");
            else
                ImGui::SetTooltip("Not available in the loaded emulator core -- rebuild it "
                                  "with scripts/build_puae to enable this.");
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Applied at write time -- takes effect on the next emulated frame.");
}

// The blit overlay: where the blitter put its pixels, drawn on the emulator picture.
//
// Two renderers, one feature. The core tags pixels as the display fetches them, which needs no
// coordinate reconstruction and so gets scrolling, modulos, fetch mode and dual playfield right;
// but it can only ever describe pixels that were scanned out. A blit into a buffer that is never
// displayed produces no spans at all, and for those -- and only those -- a rectangle rebuilt from
// bltdpt/bltdmod geometry is drawn instead. The user does not choose between them.
void DebuggerDisplayView::DrawBlitterSection()
{
    if (!BeginSection("Blitter"))
        return;

    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    const bool hasProvenance = emu && emu->HasBlitterVis();

    BlitSettings s = m_blit;
    bool changed = false;

    if (ImGui::Checkbox("Blit Overlay##blitovl", &s.enabled))
        changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Colour every pixel on screen by the blit that last wrote it, and box the "
            "destinations that never made it to screen.\n\nHover anywhere the overlay is showing "
            "to identify the blit that wrote it and highlight it in the Frame Scrubber; Alt+click "
            "to pin it and sync the trace windows to it. Selecting a blit elsewhere dims every "
            "other one here.");

    // Same reasoning as the Raster section: the section header already collapses this, so the
    // controls stay visible with the overlay off rather than hiding until it is switched on.

    // What the overlay actually managed to show. The box count is the diagnostic worth having:
    // a frame that is all boxes means the blits are landing somewhere never scanned out. The
    // unplaced count closes the account -- it only appears when something is missing, so the
    // three numbers summing to the frame's blit total stays a check you can actually run.
    ImGui::SameLine();
    if (s.useProvenance && hasProvenance) {
        if (m_blitUnplacedBlits > 0)
            ImGui::TextDisabled("(%d spans \xc2\xb7 %d as pixels, %d as boxes, %d unplaced)",
                                m_blitSpans, m_blitPixelBlits, m_blitBoxBlits, m_blitUnplacedBlits);
        else
            ImGui::TextDisabled("(%d spans \xc2\xb7 %d as pixels, %d as boxes)", m_blitSpans,
                                m_blitPixelBlits, m_blitBoxBlits);
        if (ImGui::IsItemHovered()) {
            if (m_blitUnplacedBlits > 0)
                ImGui::SetTooltip(
                    "%d blit(s) this frame are drawn by neither layer: nothing tagged their "
                    "pixels, and their destination did not resolve to a bitplane buffer. "
                    "Line-mode blits, and copies into memory that is not a display buffer, land "
                    "here. With Outline all off the three counts account for every blit in the "
                    "frame.",
                    m_blitUnplacedBlits);
            else if (m_blitSpans == 0)
                ImGui::SetTooltip(
                    "No pixels tagged yet. Either nothing has blitted since the overlay "
                    "was switched on, or every destination is off-screen -- the box "
                    "count tells you which.");
        }
    } else if (m_blitUnplacedBlits > 0) {
        ImGui::TextDisabled("(geometry only \xc2\xb7 %d boxes, %d unplaced)", m_blitBoxBlits,
                            m_blitUnplacedBlits);
    } else {
        ImGui::TextDisabled("(geometry only \xc2\xb7 %d boxes)", m_blitBoxBlits);
    }

    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::SliderFloat("Opacity##blitopacity", &s.opacity, 0.0f, 1.0f, "%.2f"))
        changed = true;

    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::SliderInt("Decay##blitdecay", &s.decayFrames, 1, adm::core::blitvis::kMaxDecayFrames,
                         "%d frames"))
        changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How long a tagged pixel keeps fading for. 1 frame makes a one-shot "
                          "blit a single-frame flicker; trailing it over a few frames is what "
                          "lets the eye catch it.");

    if (BeginTwoColumns("##blitlayers")) {
        ImGui::TableSetupColumn("##b_left", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##b_right", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        {
            // Forced off on a core without the patch: the boxes are then all there is, and a
            // checkbox that silently does nothing is worse than one that explains itself.
            ImGui::BeginDisabled(!hasProvenance);
            bool useProv = hasProvenance && s.useProvenance;
            if (ImGui::Checkbox("Pixel provenance##blitprov", &useProv)) {
                s.useProvenance = useProv;
                changed = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (hasProvenance)
                    ImGui::SetTooltip(
                        "Tag pixels in the core as the display fetches them, so the overlay is "
                        "exact rather than inferred from bltdpt geometry.\n\nCosts a per-fetch "
                        "lookup and roughly 50 MB of tables while it is on. Turn it off to fall "
                        "back to geometry boxes for every blit, which costs nothing.");
                else
                    ImGui::SetTooltip("Not available in the loaded emulator core -- rebuild it "
                                      "with scripts/build_puae to enable this. Until then the "
                                      "overlay draws geometry boxes only.");
            }
        }

        ImGui::TableSetColumnIndex(1);
        if (ImGui::Checkbox("Outline all##blitoutline", &s.outlineAll))
            changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Also outline the destinations the provenance fill already covers, so every blit "
                "in the frame gets a box.\n\nOff by default: the fill says the same thing more "
                "precisely, and a box per blit buries the picture on a busy frame. Green = an "
                "on-screen buffer, yellow = an off-screen buffer of the same geometry.");

        ImGui::EndTable();
    }

    ImGui::Separator();

    // Not an overlay setting -- it feeds the Blitter Operations window's Data column -- but it is
    // the third blitter debug switch, and having it in a different window than the other two was
    // the reason nobody found it.
    {
        ImGui::BeginDisabled(!m_captureDestSupported);
        bool cap = m_captureDestData;
        if (ImGui::Checkbox("Capture destination data##blitcap", &cap)) {
            m_captureDestData = cap;
            if (m_onCaptureDestDataChanged)
                m_onCaptureDestDataChanged(cap);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!m_captureDestSupported)
                ImGui::SetTooltip("The loaded PuAE core predates destination capture.\n"
                                  "Rebuild it with scripts/build_puae to enable this.");
            else
                ImGui::SetTooltip(
                    "Capture the words each blit writes through its D channel and show them as a "
                    "picture in the Blitter Operations window's Data column.\n\nThis is \"what "
                    "the blit drew\" where the overlay above is \"where it went\". Costs ~4 MB in "
                    "the core while on.");
        }
    }

    if (changed) {
        m_blit = s;
        if (m_onBlitOverlayChanged)
            m_onBlitOverlayChanged(m_blit);
    }
}

// The Copper / DMA raster ("beam view") controls. Turning this on adjusts the emulator window:
// the picture shrinks and is inset inside a rect standing for the whole raster, so the offscreen
// colour clocks either side of it become visible and can be painted.
//
// Adapted from the ENGINE9000 debugger by alpine9000 -- see core/CopperRaster.h and PATCHES.md.
void DebuggerDisplayView::DrawRasterSection()
{
    if (!BeginSection("Raster (Beam View)"))
        return;

    if (ImGui::Checkbox("Show Raster##raster", &m_raster.enabled)) {
        if (m_onShowCopperRasterChanged)
            m_onShowCopperRasterChanged(m_raster.enabled);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Inset the emulator picture inside the full 227x313 beam and colour every colour "
            "clock by what the bus did in it.\n\nThe Copper trace tells you what ran and the "
            "Frame Scrubber tells you when; this is the missing dimension -- where on the "
            "raster. A colour split is a horizontal line, a stalled WAIT is a horizontal bar, "
            "and a Copper that misses its slot has the culprit sitting right next to it.\n\n"
            "Click a cell to jump every debugger window to that colour clock.");

    // Everything below stays visible with the overlay off: the section header already collapses
    // it, and hiding the layer/channel switches behind the checkbox meant nobody could set the
    // overlay up before turning it on.
    ImGui::SameLine();
    ImGui::TextDisabled("(%d copper, %d DMA)", m_rasterCopperCells, m_rasterDmaCells);
    if (m_rasterDmaCells == 0 && ImGui::IsItemHovered())
        ImGui::SetTooltip("No DMA slots recorded yet. Recording is armed automatically while "
                          "this overlay is open; run a frame to populate it.");

    // Geometry readout: how big the beam is, and where the picture sits inside it. The ratio is
    // exactly how much the picture shrinks to make room for the offscreen clocks, so a picture
    // that comes out smaller than expected is diagnosable from this line alone.
    if (m_rasterMap.valid) {
        ImGui::TextDisabled("Beam %dx%d cc  \xc2\xb7  image %.0fx%.0f cc @ (%.0f,%.0f)  \xc2\xb7  "
                            "x%.2f/%.2f",
                            m_rasterMap.rasterCC, m_rasterMap.rasterLines, m_rasterMap.imageWidthCC,
                            m_rasterMap.imageHeightLines, m_rasterMap.imageLeftCC,
                            m_rasterMap.imageTopLine, m_rasterMap.RasterToImageX(),
                            m_rasterMap.RasterToImageY());
    }

    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Opacity##rasteropacity", &m_raster.opacity, 0.0f, 1.0f, "%.2f");

    // Wide enough for the longest label ("Per-line DDF/DIW"); the previous 160 clipped several.
    const float columnWidth = 200.0f;
    if (BeginTwoColumns("##rasterlayers")) {
        ImGui::TableSetupColumn("##r_left", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##r_right", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("Copper##rcop", &m_raster.showCopper);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Colour each Copper instruction by what it targets: WAIT, SKIP, "
                              "and MOVEs to sprite / blitter / bitplane pointer / colour / other "
                              "registers.");

        ImGui::TableSetColumnIndex(1);
        ImGui::Checkbox("WAIT Stalls##rwait", &m_raster.showWaitSpans);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shade the run between a WAIT and the cycle the Copper actually "
                              "woke up on. This is the time the Copper spent blocked -- a bar "
                              "the one-dimensional trace cannot show.");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("Bus Denied##rwant", &m_raster.showCopperWanted);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Mark cycles where the Copper asked for the bus and did not get "
                              "it. This is what makes a colour split jitter, and it is not "
                              "visible anywhere else in the debugger.");

        ImGui::TableSetColumnIndex(1);
        ImGui::Checkbox("Blanking##rblank", &m_raster.showBlanking);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shade the horizontal/vertical blanking and sync regions, so the "
                              "offscreen margin reads as structure rather than empty space.");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("Per-line DDF/DIW##rgeo", &m_raster.showGeometryEvents);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Mark where DDFSTRT/DDFSTOP and the display window actually fired "
                              "on each line.\n\nThe Geometry overlay draws one box from the "
                              "current register values -- the end-of-frame state flattened over "
                              "the whole screen. These are per line, so a Copper-driven change "
                              "shows up as a stepped edge instead.");

        ImGui::EndTable();
    }

    // Per-channel visibility. Channel order, colours and labels come from DmaColors.h so this
    // reads the same as the DMA Stats window and the Frame Scrubber's DMA row.
    static const int kTypes[] = {
        adm::AmigaEmulator::kDmaRecordBitplane, adm::AmigaEmulator::kDmaRecordCopper,
        adm::AmigaEmulator::kDmaRecordBlitter,  adm::AmigaEmulator::kDmaRecordSprite,
        adm::AmigaEmulator::kDmaRecordCpu,      adm::AmigaEmulator::kDmaRecordAudio,
        adm::AmigaEmulator::kDmaRecordDisk,     adm::AmigaEmulator::kDmaRecordRefresh,
        adm::AmigaEmulator::kDmaRecordConflict};
    constexpr uint16_t kAllChannels = 0x03FE;

    {
        ImGui::Spacing();
        // Master toggle, same three-state behaviour as "All Bitplanes".
        {
            const uint16_t masked = static_cast<uint16_t>(m_raster.channelMask & kAllChannels);
            bool allOn = (masked == kAllChannels);
            const bool mixed = (masked != 0 && masked != kAllChannels);
            if (mixed)
                ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            if (ImGui::Checkbox("All DMA Channels##rchall", &allOn))
                m_raster.channelMask = allOn ? kAllChannels : 0;
            if (mixed)
                ImGui::PopItemFlag();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Paint DMA slots on the raster, using the same channel colours as "
                              "the Frame Scrubber's DMA row and the DMA Stats window.\n\n"
                              "Dense: a slot lands on nearly every colour clock, so this is off "
                              "by default and the picture stays readable underneath.");

        ImGui::Spacing();

        if (BeginTwoColumns("##rchtable")) {
            ImGui::TableSetupColumn("##rch_left", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##rch_right", ImGuiTableColumnFlags_WidthStretch);

            int col = 0;
            for (int type : kTypes) {
                if (col % 2 == 0)
                    ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(col % 2);
                ++col;

                const uint16_t bit = static_cast<uint16_t>(1u << type);
                bool on = (m_raster.channelMask & bit) != 0;

                ImGui::PushID(type);
                ImGui::ColorButton("##swatch", ImGui::ColorConvertU32ToFloat4(DmaTypeColor(type)),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                                   ImVec2(12, 12));
                ImGui::SameLine();
                if (ImGui::Checkbox(DmaTypeLabel(type), &on))
                    m_raster.channelMask = on ? static_cast<uint16_t>(m_raster.channelMask | bit)
                                              : static_cast<uint16_t>(m_raster.channelMask & ~bit);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // Legend for the copper classes. The DMA channel key lives in the DMA Stats window, which
    // already renders it with live per-channel totals -- no point drawing those numbers twice.
    if (m_raster.showCopper && ImGui::TreeNode("Copper Key##rkey")) {
        struct Entry
        {
            adm::core::copraster::CopClass klass;
            const char* label;
        };
        static const Entry kEntries[] = {
            {adm::core::copraster::CopClass::Wait, "WAIT"},
            {adm::core::copraster::CopClass::Skip, "SKIP"},
            {adm::core::copraster::CopClass::MoveColor, "MOVE COLORxx"},
            {adm::core::copraster::CopClass::MoveBplPtr, "MOVE BPLxPT"},
            {adm::core::copraster::CopClass::MoveSprite, "MOVE SPRxxx"},
            {adm::core::copraster::CopClass::MoveBlitter, "MOVE BLTxxx"},
            {adm::core::copraster::CopClass::MoveOther, "MOVE other"},
        };
        const auto pal = adm::core::copraster::Palette::Default();

        if (BeginTwoColumns("##rkeytable")) {
            ImGui::TableSetupColumn("##rkey_left", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##rkey_right", ImGuiTableColumnFlags_WidthStretch);

            int col = 0;
            for (const auto& e : kEntries) {
                if (col % 2 == 0)
                    ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(col % 2);
                ++col;

                ImGui::ColorButton(e.label,
                                   ImGui::ColorConvertU32ToFloat4(static_cast<ImU32>(
                                       pal.copper[static_cast<size_t>(e.klass)])),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                                   ImVec2(12, 12));
                ImGui::SameLine();
                ImGui::TextUnformatted(e.label);
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

void DebuggerDisplayView::DrawOverlays(ImDrawList* dl, ImVec2 screenMin, ImVec2 screenMax)
{
    if (m_showFrameCounterOverlay && m_emulatorView && m_emulatorView->IsRunning()) {
        std::string frameText =
            "Frame: " + std::to_string(m_emulatorView->GetEmulator()->GetTotalFrameCount());
        ImVec2 textSize = ImGui::CalcTextSize(frameText.c_str());
        ImVec2 textPos = ImVec2(screenMax.x - textSize.x - 8.0f, screenMax.y - textSize.y - 8.0f);

        dl->AddRectFilled(ImVec2(textPos.x - 6.0f, textPos.y - 4.0f),
                          ImVec2(textPos.x + textSize.x + 6.0f, textPos.y + textSize.y + 4.0f),
                          IM_COL32(0, 0, 0, 180), 4.0f);
        dl->AddRect(ImVec2(textPos.x - 6.0f, textPos.y - 4.0f),
                    ImVec2(textPos.x + textSize.x + 6.0f, textPos.y + textSize.y + 4.0f),
                    IM_COL32(80, 80, 80, 120), 4.0f);
        dl->AddText(textPos, IM_COL32(255, 255, 255, 255), frameText.c_str());
    }

    const bool showGeo = m_showGeometryOverlay;
    const bool showMag = m_emulatorView && m_emulatorView->IsMagnifierEnabled();
    if ((!showGeo && !showMag) || !m_emulatorView || !m_emulatorView->IsRunning())
        return;

    adm::core::Custom regs{};
    if (!m_emulatorView->GetEmulator()->GetCustomRegisters(regs))
        return;

    // --- Coordinate mapping ---
    // Use hardware frame geometry to correctly map Amiga CCK coordinates onto the
    // (potentially cropped) texture that is displayed on screen.
    auto geom = m_emulatorView->GetEmulator()->GetDisplayGeometry();
    float texWidth = static_cast<float>(m_emulatorView->GetEmulator()->GetWidth());
    float texHeight = static_cast<float>(m_emulatorView->GetEmulator()->GetHeight());

    // Shared mapping (same one the blit-destination overlay uses, so they align exactly).
    core::AmigaScreenMap smap =
        core::AmigaScreenMap::Build(geom, static_cast<int>(texWidth), static_cast<int>(texHeight));
    float loresScale = smap.loresScale;
    float vScale = smap.vScale;
    float loresTexWidth = smap.loresTexW;
    float scanTexHeight = smap.scanTexH;
    int hbase = smap.hbase;
    int vbase = smap.vbase;

    float loresToScreen = (screenMax.x - screenMin.x) / loresTexWidth;
    float scanToScreen = (screenMax.y - screenMin.y) / scanTexHeight;

    auto amigaToScreen = [&](int hpix, int vscan) -> ImVec2 {
        return {screenMin.x + static_cast<float>(hpix - hbase) * loresToScreen,
                screenMin.y + static_cast<float>(vscan - vbase) * scanToScreen};
    };

    const ImU32 white = IM_COL32(255, 255, 255, 230);

    // DIW/DDF register positions are always expressed in lores pixels, but the screen the program
    // actually draws is in its own resolution: a hires screen fits 2 pixels, a super-hires screen
    // 4, into each lores position. Report sizes and pixel coordinates in those native pixels so a
    // 640x256 hires screen reads as 640x256 and not 320x256.
    //
    // This is the DISPLAY's resolution (BPLCON0), which is independent of the emulator texture's
    // resolution (AmigaScreenMap::loresScale) — PuAE can render a hires screen into a lores
    // framebuffer and vice versa.
    const core::BitplaneResolution res = core::ResolutionFromBplcon0(regs.bplcon0);
    const int pxPerLores = core::ResolutionPixelsPerLores(res);
    const bool interlaced = (regs.bplcon0 & 0x0004) != 0;
    const int linesPerScan = interlaced ? 2 : 1;

    // --- Geometry overlay: DIW / DDF boundaries ---
    if (showGeo) {
        // --- Decode DIW ---
        int diw_h_start = regs.diwstrt & 0xFF;
        int diw_v_start = (regs.diwstrt >> 8) & 0xFF;
        int diw_h_stop = (regs.diwstop & 0xFF) | 0x100; // OCS: implicit bit-8 on h-stop
        int diw_v_stop_raw = (regs.diwstop >> 8) & 0xFF;
        // OCS hardware rule (matches UAE calcdiw): if bit 7 of the 8-bit vstop value
        // is 0, the actual stop line is in the 256-511 range (add 0x100).
        // e.g. PAL 256-line: raw=44 (0x2C, bit7=0) → actual 300
        //      screenshot:   raw=64 (0x40, bit7=0) → actual 320
        //      NTSC 200-line: raw=244 (0xF4, bit7=1) → stays at 244
        int diw_v_stop = diw_v_stop_raw;
        if ((diw_v_stop_raw & 0x80) == 0)
            diw_v_stop |= 0x100;

        // --- Decode DDF: registers store position in increments of 2 CCKs; multiply by 2 to get
        // CCK position ---
        int ddf_start_px = (regs.ddfstrt & 0xFE) * 2;
        int ddf_stop_px = (regs.ddfstop & 0xFE) * 2;

        // --- Draw DIW filled rect + border ---
        ImVec2 diwTL = amigaToScreen(diw_h_start, diw_v_start);
        ImVec2 diwBR = amigaToScreen(diw_h_stop, diw_v_stop);

        const ImU32 diwFill = IM_COL32(60, 120, 255, 60);
        const ImU32 diwBorder = IM_COL32(60, 120, 255, 220);
        dl->AddRectFilled(diwTL, diwBR, diwFill);
        dl->AddRect(diwTL, diwBR, diwBorder, 0.0f, 0, 1.5f);

        // --- DIW labels ---
        {
            char buf[48];
            // Top-left corner label
            snprintf(buf, sizeof(buf), "(%d,%d)", diw_h_start, diw_v_start);
            dl->AddText({diwTL.x + 3.0f, diwTL.y + 2.0f}, white, buf);

            // Bottom-right corner label (offset inward)
            snprintf(buf, sizeof(buf), "(%d,%d)", diw_h_stop, diw_v_stop);
            ImVec2 brTextPos = {diwBR.x - ImGui::CalcTextSize(buf).x - 3.0f,
                                diwBR.y - ImGui::GetTextLineHeight() - 2.0f};
            dl->AddText(brTextPos, white, buf);

            // Center label: DIW dimensions in the screen's own pixels
            int diwW = (diw_h_stop - diw_h_start) * pxPerLores;
            int diwH = (diw_v_stop - diw_v_start) * linesPerScan;
            snprintf(buf, sizeof(buf), "DIW: %dx%d %s%s", diwW, diwH, core::ResolutionName(res),
                     interlaced ? " lace" : "");
            ImVec2 center = {(diwTL.x + diwBR.x) * 0.5f - ImGui::CalcTextSize(buf).x * 0.5f,
                             (diwTL.y + diwBR.y) * 0.5f - ImGui::GetTextLineHeight() * 0.5f};
            dl->AddText(center, white, buf);
        }

        // --- Draw DDF dashed vertical lines ---
        const ImU32 orange = IM_COL32(255, 165, 0, 220);
        const float dashLen = 6.0f;
        const float gapLen = 4.0f;

        auto drawDashedVertical = [&](float x, ImU32 col) {
            float y = screenMin.y;
            while (y < screenMax.y) {
                float yEnd = std::min(y + dashLen, screenMax.y);
                dl->AddLine({x, y}, {x, yEnd}, col, 1.5f);
                y += dashLen + gapLen;
            }
        };

        ImVec2 ddfStartScreen = amigaToScreen(ddf_start_px, 0);
        ImVec2 ddfStopScreen = amigaToScreen(ddf_stop_px, 0);
        drawDashedVertical(ddfStartScreen.x, orange);
        drawDashedVertical(ddfStopScreen.x, orange);

        // --- DDF labels ---
        {
            char buf[32];
            // Below DDF start line (show CCK position = raw register × 2)
            snprintf(buf, sizeof(buf), "DDFSTRT: %d", ddf_start_px);
            dl->AddText({ddfStartScreen.x + 3.0f, screenMax.y - ImGui::GetTextLineHeight() - 3.0f},
                        orange, buf);

            // Above DDF stop line (show CCK position = raw register × 2)
            snprintf(buf, sizeof(buf), "DDFSTOP: %d", ddf_stop_px);
            dl->AddText({ddfStopScreen.x + 3.0f, screenMin.y + 3.0f}, orange, buf);

            // Above DIW box: DDF fetch width, in the screen's own pixels.
            // DDFSTOP marks the START of the last fetch word, so the span covers 16 extra lores
            // pixels. That +16 holds in lores units for every resolution: hires/super-hires fetch
            // more words over the same span rather than covering more lores pixels, so scaling
            // the lores span by pxPerLores yields the native width (e.g. $3C/$D4 → 640 hires px).
            int ddfW = (ddf_stop_px - ddf_start_px + 16) * pxPerLores;
            snprintf(buf, sizeof(buf), "DDF: %dpx", ddfW);
            float ddfLabelX =
                (ddfStartScreen.x + ddfStopScreen.x) * 0.5f - ImGui::CalcTextSize(buf).x * 0.5f;
            dl->AddText({ddfLabelX, diwTL.y - ImGui::GetTextLineHeight() - 3.0f}, orange, buf);
        }
    }

    // Emulator framebuffer used by the magnifier loupe.
    GLuint emTex = m_emulatorView->GetEmulator()->GetTextureID();
    int emTexW = static_cast<int>(m_emulatorView->GetEmulator()->GetWidth());
    int emTexH = static_cast<int>(m_emulatorView->GetEmulator()->GetHeight());

    // Integer texels-per-lores-pixel (the texture may be hi-res internally).
    int loresScaleI = std::max(1, static_cast<int>(loresScale));
    int vScaleI = std::max(1, static_cast<int>(vScale));

    // Size of the loupe's highlighted "unit" cell, in texels: one pixel of the screen the program
    // is actually drawing. A hires screen packs 2 of its pixels into each lores position and a
    // super-hires screen 4, so the cell shrinks accordingly — highlighting a whole lores pixel
    // would box 2 (or 4) real pixels and make individual hires pixels impossible to identify.
    // The texture can't resolve finer than one texel, so clamp there.
    const int hlTexelsW = std::max(1, loresScaleI / pxPerLores);
    const int hlTexelsH = std::max(1, vScaleI / linesPerScan);

    // The loupe magnifies a 20×10 lores-pixel window at 16 px per lores pixel (320×160).
    const float kLoupeW = 20.0f * 16.0f;

    auto amigaCoordFromScreen = [&](const ImVec2& p, int& hpix, int& vscan) {
        hpix = hbase + static_cast<int>((p.x - screenMin.x) / loresToScreen);
        vscan = vbase + static_cast<int>((p.y - screenMin.y) / scanToScreen);
    };

    ImVec2 mouse = ImGui::GetMousePos();
    bool mouseInScreen = mouse.x >= screenMin.x && mouse.x <= screenMax.x &&
                         mouse.y >= screenMin.y && mouse.y <= screenMax.y;

    // --- Geometry crosshair (click-to-lock) — geometry overlay only ---
    if (showGeo) {
        // Right/middle click anywhere in the screen area clears the lock
        if (m_crosshairLocked && mouseInScreen) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
                m_crosshairLocked = false;
            }
        }

        if (m_crosshairLocked) {
            // --- Locked crosshair ---
            ImVec2 crossPos = amigaToScreen(m_lockedHpix, m_lockedVscan);
            const ImU32 lockColor = IM_COL32(255, 200, 50, 220);

            if (crossPos.y >= screenMin.y && crossPos.y <= screenMax.y)
                dl->AddLine({screenMin.x, crossPos.y}, {screenMax.x, crossPos.y}, lockColor, 1.5f);
            if (crossPos.x >= screenMin.x && crossPos.x <= screenMax.x)
                dl->AddLine({crossPos.x, screenMin.y}, {crossPos.x, screenMax.y}, lockColor, 1.5f);

            // Small filled circle at the intersection to mark the lock point
            dl->AddCircleFilled(crossPos, 4.0f, lockColor);
            dl->AddCircle(crossPos, 4.0f, IM_COL32(255, 255, 255, 200), 0, 1.0f);

            // Info box drawn via DrawList (pinned near the crosshair, not following the mouse)
            const BeamPos lockedBeam = BeamFromDenise(m_lockedHpix, m_lockedVscan);
            const uint32_t lRef = LRefFromDenise(m_lockedHpix, m_lockedVscan);

            // Denise H / Agnus CCK / L-Ref are hardware coordinate systems and stay in lores
            // and color-clock units. On a finer screen that leaves the reader to do the
            // conversion, so the program's own pixel column is spelled out alongside them —
            // matching the hover tooltip. The lock itself is lores-granular (L-Ref is a CCK),
            // so this names the leftmost native pixel of the locked column.
            char lines[4][64];
            int lineCount = 0;
            snprintf(lines[lineCount++], sizeof(lines[0]), "Denise H:%d V:%d", m_lockedHpix,
                     m_lockedVscan);
            if (pxPerLores > 1) {
                snprintf(lines[lineCount++], sizeof(lines[0]), "%s H:%d", core::ResolutionName(res),
                         m_lockedHpix * pxPerLores);
            }
            snprintf(lines[lineCount++], sizeof(lines[0]), "Agnus Line:%d CCK:%d", lockedBeam.line,
                     lockedBeam.cck);
            snprintf(lines[lineCount++], sizeof(lines[0]), "L-Ref: L%07X", lRef);

            const float lineH = ImGui::GetTextLineHeight();
            const float pad = 4.0f;
            float textW = 0.0f;
            for (int i = 0; i < lineCount; i++)
                textW = std::max(textW, ImGui::CalcTextSize(lines[i]).x);
            const float boxW = textW + pad * 2.0f;
            const float boxH = lineH * static_cast<float>(lineCount) + pad * 2.0f + 2.0f;

            // Prefer upper-right of crosshair; clamp so the box stays inside the screen
            float bx = crossPos.x + 10.0f;
            float by = crossPos.y - boxH - 6.0f;
            bx = std::clamp(bx, screenMin.x + 1.0f,
                            std::max(screenMin.x + 1.0f, screenMax.x - boxW - 1.0f));
            by = std::clamp(by, screenMin.y + 1.0f,
                            std::max(screenMin.y + 1.0f, screenMax.y - boxH - 1.0f));

            dl->AddRectFilled({bx, by}, {bx + boxW, by + boxH}, IM_COL32(20, 20, 30, 210), 3.0f);
            dl->AddRect({bx, by}, {bx + boxW, by + boxH}, lockColor, 3.0f, 0, 1.0f);
            for (int i = 0; i < lineCount; i++) {
                dl->AddText({bx + pad, by + pad + (lineH + 1.0f) * static_cast<float>(i)}, white,
                            lines[i]);
            }

        } else if (mouseInScreen) {
            // --- Follow-mouse crosshair ---
            int hpix, vscan;
            amigaCoordFromScreen(mouse, hpix, vscan);

            // Left-click: lock here and sync all linked views
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                m_lockedHpix = hpix;
                m_lockedVscan = vscan;
                m_crosshairLocked = true;
                const uint32_t lRef = LRefFromDenise(hpix, vscan);
                if (m_onJumpToLRef)
                    m_onJumpToLRef(lRef);
            }

            const ImU32 crossColor = IM_COL32(255, 255, 100, 140);
            dl->AddLine({screenMin.x, mouse.y}, {screenMax.x, mouse.y}, crossColor, 1.0f);
            dl->AddLine({mouse.x, screenMin.y}, {mouse.x, screenMax.y}, crossColor, 1.0f);
        }
    }

    // --- Hover tooltip: magnifier loupe (if enabled) + pixel coordinates ---
    // Reports the position under the mouse so the loupe can be explored freely even while the
    // geometry crosshair is locked elsewhere.
    if (mouseInScreen && (showGeo || showMag)) {
        int mhpix, mvscan;
        amigaCoordFromScreen(mouse, mhpix, mvscan);
        const BeamPos mBeam = BeamFromDenise(mhpix, mvscan);
        const uint32_t mLRef = LRefFromDenise(mhpix, mvscan);

        if (showMag)
            ImGui::SetNextWindowSizeConstraints(ImVec2(kLoupeW, 0), ImVec2(kLoupeW, FLT_MAX));
        ImGui::BeginTooltip();
        if (showMag && emTex != 0 && emTexW > 0 && emTexH > 0) {
            // Locate the loupe by TEXEL, not by lores pixel: deriving the centre from the (integer)
            // lores coordinate quantised the crosshair to 2-texel steps on a hires texture, so odd
            // hires pixels were unreachable. Snap to the unit cell so the highlight still lands on
            // real display-pixel boundaries.
            const float texelScreenX = (screenMax.x - screenMin.x) / static_cast<float>(emTexW);
            const float texelScreenY = (screenMax.y - screenMin.y) / static_cast<float>(emTexH);
            int mTexX = static_cast<int>((mouse.x - screenMin.x) / texelScreenX);
            int mTexY = static_cast<int>((mouse.y - screenMin.y) / texelScreenY);
            mTexX = (mTexX / hlTexelsW) * hlTexelsW;
            mTexY = (mTexY / hlTexelsH) * hlTexelsH;

            MagnifierStyle style;
            style.texelScreenW = 16.0f / static_cast<float>(loresScaleI);
            style.texelScreenH = 16.0f / static_cast<float>(vScaleI);
            DrawTextureLoupe(emTex, emTexW, emTexH, mTexX, mTexY, 20 * loresScaleI, 10 * vScaleI,
                             hlTexelsW, hlTexelsH, style);
            ImGui::Separator();
        }
        ImGui::Text("Denise H:%d V:%d", mhpix, mvscan);
        if (pxPerLores > 1) {
            // Same origin as Denise H, expressed in the pixels the program is drawing.
            int mhpixNative =
                hbase * pxPerLores + static_cast<int>((mouse.x - screenMin.x) / loresToScreen *
                                                      static_cast<float>(pxPerLores));
            ImGui::Text("%s H:%d", core::ResolutionName(res), mhpixNative);
        }
        ImGui::Text("Agnus Line:%d CCK:%d", mBeam.line, mBeam.cck);
        ImGui::Text("L-Ref: L%07X", mLRef);
        ImGui::EndTooltip();
    }
}

} // namespace adm::ui
