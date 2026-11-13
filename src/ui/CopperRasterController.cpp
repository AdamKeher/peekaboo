#include "CopperRasterController.h"

#include <cstdio>

#include <imgui.h>

#include "../core/AmigaScreenMap.h"
#include "../core/Constants.h"
#include "../core/CopperRaster.h"
#include "../core/Logger.h"
#include "../core/RasterFrameMap.h"
#include "DmaColors.h"
#include "views/DebuggerView.h"
#include "views/EmulatorView.h"

namespace adm::ui {

void CopperRasterController::Init(EmulatorView* emulatorView, DebuggerView* debuggerView)
{
    m_emulatorView = emulatorView;
    m_debuggerView = debuggerView;
}

// Arms/disarms the core's DMA recording for the raster and releases its texture when off.
// Mode 1 records without PuAE drawing its own overlay into the framebuffer (drawing.c gates that
// on debug_dma > 1), which is the same mode DmaStatsView uses.
void CopperRasterController::SetEnabled(bool enabled)
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (emu && emu->HasDmaDebug() && enabled)
        emu->SetDebugDma(1);

    if (!enabled) {
        m_copperRaster.Reset(); // don't hold a raster-sized buffer while switched off
        if (m_emulatorView)
            m_emulatorView->SetRasterFrameMode(false);
        // Deliberately NOT turning debug_dma off here: DmaStatsView may still be open and using
        // it. Whichever consumer closes last leaves it armed, which costs a recording pass and
        // nothing else; both re-arm it every frame anyway.
    }
}

void CopperRasterController::Update(AmigaEmulator* emu, const AmigaEmulator::DmaSlotRecord* records,
                                    int hposCount, int vposCount,
                                    const std::vector<AmigaEmulator::CopperExecEvent>& copperEvents)
{
    if (!emu || !m_debuggerView || !m_emulatorView)
        return;

    const auto& settings = m_debuggerView->GetRasterSettings();
    if (!settings.enabled) {
        m_emulatorView->SetRasterFrameMode(false);
        return;
    }

    // DMA recording is armed on the toggle, but an emulator restart resets debug_dma back to 0.
    // Re-check every frame and recover, the same discipline DmaStatsView uses.
    if (emu->HasDmaDebug() && emu->GetDebugDma() == 0)
        emu->SetDebugDma(1);

    // Hold the last known extent so a frame without a record (recording only just armed) keeps
    // the window geometry steady instead of collapsing the raster to nothing.
    // vposCount is the record BUFFER height (NR_DMA_REC_VPOS = 1000), not the frame height.
    // Derive the real one, or the raster ends up three times too tall and the picture is squashed
    // into a corner of it.
    const int frameLines =
        core::copraster::DeriveFrameHeight(records, hposCount, vposCount, kDefaultFrameLines);
    if (hposCount > 0 && frameLines > 0) {
        m_rasterHposCount = hposCount;
        m_rasterVposCount = frameLines;
    }
    // Before the first record arrives, fall back to a full PAL frame. Only ever transient.
    const int extentH = (m_rasterHposCount > 0)
                            ? m_rasterHposCount
                            : static_cast<int>(adm::core::constants::beam::CCKPerLine);
    const int extentV = (m_rasterVposCount > 0) ? m_rasterVposCount : kDefaultFrameLines;

    const auto smap =
        core::AmigaScreenMap::Build(emu->GetDisplayGeometry(), static_cast<int>(emu->GetWidth()),
                                    static_cast<int>(emu->GetHeight()));
    const auto map = core::RasterFrameMap::Build(smap, extentH, extentV);
    if (!map.valid) {
        m_emulatorView->SetRasterFrameMode(false);
        return;
    }

    auto& opt = m_copperRaster.MutableOptions();
    opt.showCopper = settings.showCopper;
    opt.showWaitSpans = settings.showWaitSpans;
    opt.showCopperWanted = settings.showCopperWanted;
    opt.showBlanking = settings.showBlanking;
    opt.showGeometryEvents = settings.showGeometryEvents;
    opt.channelMask = settings.channelMask;
    // Dim the region the beam had not reached when stopped mid-frame, matching the scrubber.
    opt.breakLRef =
        emu->IsPartialFrame()
            ? (static_cast<uint32_t>(emu->GetBreakVpos()) * adm::core::constants::beam::CCKPerLine +
               static_cast<uint32_t>(emu->GetBreakHpos()))
            : UINT32_MAX;
    m_copperRaster.SetOpacity(settings.opacity);

    core::copraster::Input in;
    in.records = records;
    in.hposCount = hposCount;
    in.vposCount = (records && frameLines > 0) ? frameLines : vposCount;
    in.copperEvents = copperEvents.empty() ? nullptr : copperEvents.data();
    in.copperEventCount = static_cast<int>(copperEvents.size());
    // With no DMA record the copper layer still needs to know how tall the frame is.
    if (in.vposCount == 0)
        in.vposCount = extentV;

    m_copperRaster.Update(map, in);
    m_debuggerView->SetRasterStats(m_copperRaster.GetCopperCellCount(),
                                   m_copperRaster.GetDmaCellCount());
    m_debuggerView->SetRasterGeometry(map);

    // Log the layout once whenever it changes. "The picture is the wrong size" is otherwise an
    // unattributable complaint; these are the six numbers that determine it.
    static int lastCC = -1, lastLines = -1, lastW = -1, lastH = -1;
    const int curW = static_cast<int>(map.imageWidthCC);
    const int curH = static_cast<int>(map.imageHeightLines);
    if (map.rasterCC != lastCC || map.rasterLines != lastLines || curW != lastW || curH != lastH) {
        lastCC = map.rasterCC;
        lastLines = map.rasterLines;
        lastW = curW;
        lastH = curH;
        core::Logger::Info(
            "Raster: beam {}x{} cc, image {:.1f}x{:.1f} cc @ ({:.1f},{:.1f}), "
            "shrink x{:.2f}/{:.2f}; tex {}x{} geom fullW={} fullH={} cropX={} cropY={} "
            "vlb={} yadj={}",
            map.rasterCC, map.rasterLines, map.imageWidthCC, map.imageHeightLines, map.imageLeftCC,
            map.imageTopLine, map.RasterToImageX(), map.RasterToImageY(), emu->GetWidth(),
            emu->GetHeight(), emu->GetDisplayGeometry().fullW, emu->GetDisplayGeometry().fullH,
            emu->GetDisplayGeometry().cropX, emu->GetDisplayGeometry().cropY,
            emu->GetDisplayGeometry().visibleLeftBorder, emu->GetDisplayGeometry().frameYAdjust);
    }

    m_emulatorView->SetRasterFrameMap(map);
    m_emulatorView->SetRasterFrameMode(true);
}

void CopperRasterController::DrawOverlay(ImDrawList* dl, ImVec2 rasterMin, ImVec2 rasterMax,
                                         ImVec2 imgMin, ImVec2 imgMax)
{
    (void)imgMin;
    (void)imgMax;
    if (!m_debuggerView || !m_debuggerView->IsRasterOverlayVisible())
        return;

    m_copperRaster.Draw(dl, rasterMin, rasterMax);

    // The scrubber playhead sweeps the raster as it is dragged -- the beam position made
    // physical, which is the whole reason to lay the frame out two-dimensionally.
    m_copperRaster.SetPlayheadLRef(m_debuggerView->GetScrubberLRef());
    m_copperRaster.DrawMarkers(dl, rasterMin, rasterMax);

    // Submitted unconditionally: an ImGui popup must be begun every frame from the window that
    // opened it, and the hover gate below returns early as soon as the mouse leaves the raster.
    DrawContextMenu();

    // Hit-testing. The overlay only reacts while the emulator window is actually hovered, so it
    // cannot steal input from a window drawn over the top of it.
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        return;

    int cc = -1, line = -1;
    const auto* cell =
        m_copperRaster.CellAt(ImGui::GetIO().MousePos, rasterMin, rasterMax, cc, line);
    if (cc < 0 || line < 0)
        return;

    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (!ImGui::IsPopupOpen(kRasterContextPopup))
        DrawTooltip(cc, line);

    const uint32_t lRef = static_cast<uint32_t>(line) * adm::core::constants::beam::CCKPerLine + cc;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // JumpToLRef is the hub: scrubber, copper trace, disassembly and the display crosshair
        // all follow. A copper cell additionally opens the trace on that exact instruction.
        m_debuggerView->JumpToLRef(lRef);
        if (cell && cell->copperEvent >= 0)
            m_debuggerView->JumpToCopperTrace(lRef);
    }

    // Right-click opens a context menu rather than arming immediately: a one-shot beam breakpoint
    // stops the machine, which is too abrupt to trigger on a stray click, and the menu also makes
    // the feature discoverable. The clicked cell is latched because the mouse will have moved by
    // the time the popup is used.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_rasterCtxLine = line;
        m_rasterCtxCC = cc;
        ImGui::OpenPopup(kRasterContextPopup);
    }
}

void CopperRasterController::DrawContextMenu()
{
    if (!ImGui::BeginPopup(kRasterContextPopup))
        return;

    const int line = m_rasterCtxLine;
    const int cc = m_rasterCtxCC;
    ImGui::TextDisabled("Line %d, Clock %d", line, cc);
    ImGui::Separator();

    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    const bool beamAvail = emu && emu->HasBeamBreak();

    ImGui::BeginDisabled(!beamAvail);
    char label[96];
    snprintf(label, sizeof(label), "Break at line %d, clock %d", line, cc);
    if (ImGui::MenuItem(label)) {
        // Goes through BreakpointsView::SetBeamBreak — the same call the Breakpoints window's own
        // controls use — so the armed breakpoint shows up there rather than being set invisibly.
        m_debuggerView->SetBeamBreakAt(line, cc);
        m_debuggerView->RevealBreakpointsWindow();
    }
    snprintf(label, sizeof(label), "Break anywhere on line %d", line);
    if (ImGui::MenuItem(label)) {
        m_debuggerView->SetBeamBreakAt(line, -1);
        m_debuggerView->RevealBreakpointsWindow();
    }
    ImGui::EndDisabled();
    if (!beamAvail)
        ImGui::TextDisabled("(needs a rebuilt core)");

    int curVpos = -1, curHpos = -1;
    m_debuggerView->GetBeamBreak(curVpos, curHpos);
    if (curVpos >= 0) {
        ImGui::Separator();
        if (curHpos >= 0)
            ImGui::TextDisabled("Armed: line %d, clock %d", curVpos, curHpos);
        else
            ImGui::TextDisabled("Armed: line %d", curVpos);
        if (ImGui::MenuItem("Clear beam breakpoint")) {
            m_debuggerView->SetBeamBreakAt(-1, -1);
            m_debuggerView->RevealBreakpointsWindow();
        }
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Open Breakpoints window"))
        m_debuggerView->RevealBreakpointsWindow();

    ImGui::EndPopup();
}

void CopperRasterController::DrawTooltip(int cc, int line)
{
    const auto& result = m_copperRaster.GetResult();
    const auto* cell = result.At(cc, line);
    const uint32_t lRef = static_cast<uint32_t>(line) * adm::core::constants::beam::CCKPerLine + cc;

    // A copper cell defers entirely to the Copper trace's own tooltip, so the raster and the
    // trace can never describe the same instruction differently.
    if (cell && cell->copperEvent >= 0 &&
        m_debuggerView->DrawCopperEventTooltip(cell->copperEvent, lRef))
        return;

    ImGui::BeginTooltip();
    ImGui::Text("Line %d, Clock %d  \xc2\xb7  L%07X", line, cc, lRef);

    if (cell && cell->dmaType != 0) {
        ImGui::Separator();
        ImVec4 col = ImGui::ColorConvertU32ToFloat4(ui::DmaTypeColor(cell->dmaType));
        ImGui::ColorButton("##ch", col,
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                           ImVec2(12, 12));
        ImGui::SameLine();
        ImGui::TextUnformatted(ui::DmaTypeLabel(cell->dmaType));

        // The record carries the register and address the slot touched; both are worth more
        // than the channel name on its own.
        if (cell->dmaSlot >= 0) {
            const AmigaEmulator::DmaSlotRecord* records = nullptr;
            int hp = 0, vp = 0;
            auto* emu = m_emulatorView->GetEmulator();
            const bool ok =
                emu && (emu->IsPartialFrame()
                            ? emu->GetDmaRecordLive(
                                  const_cast<AmigaEmulator::DmaSlotRecord**>(&records), hp, vp)
                            : emu->GetDmaRecord(
                                  const_cast<AmigaEmulator::DmaSlotRecord**>(&records), hp, vp));
            if (ok && records) {
                const auto& r = records[cell->dmaSlot];
                if (r.reg != 0xFFFF)
                    ImGui::Text("Register: $%03X", r.reg & 0x1FFu);
                ImGui::Text("Address:  $%08X", r.addr);
            }
        }
    } else {
        ImGui::TextDisabled("Bus idle");
    }

    if (cell && cell->waitBlocked)
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Copper stalled on a WAIT");
    if (cell && cell->copperWanted)
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.0f, 1.0f), "Copper wanted the bus, denied");

    ImGui::Separator();
    ImGui::TextDisabled("Click to sync all windows here \xc2\xb7 right-click to break");
    ImGui::EndTooltip();
}

} // namespace adm::ui
