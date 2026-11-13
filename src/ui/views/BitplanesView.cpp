#include "BitplanesView.h"
#include "../TableLayout.h"

#include <glad/glad.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "../../core/BlitScreenMapper.h"
#include "../AddressLink.h"
#include "../HyperlinkText.h"
#include "../OcsColor.h"
#include "EmulatorView.h"

// Register offsets used when replaying copper events for palette assignment
static constexpr uint16_t kColor00Reg = 0x180u;
static constexpr uint16_t kColor31Reg = 0x1BEu;

namespace adm::ui {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

GLuint BitplanesView::UploadRGBATexture(GLuint existing, int w, int h, const uint8_t* rgba,
                                        int prevW, int prevH)
{
    GLuint tex = existing;
    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    if (existing && w == prevW && h == prevH)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

BitplanesView::BitplanesView(EmulatorView* emulatorView) : m_emulatorView(emulatorView) {}

BitplanesView::~BitplanesView()
{
    for (int i = 0; i < kMaxPlanes; i++) {
        if (m_planeTex[i]) {
            glDeleteTextures(1, &m_planeTex[i]);
            m_planeTex[i] = 0;
        }
    }
    if (m_compositeTex) {
        glDeleteTextures(1, &m_compositeTex);
        m_compositeTex = 0;
    }
    for (auto& inst : m_traceInstances) {
        if (inst.texId) {
            glDeleteTextures(1, &inst.texId);
            inst.texId = 0;
        }
    }
}

void BitplanesView::Clear()
{
    m_regsValid = false;
    m_lastDecodeEmuFrame = UINT64_MAX; // force a re-decode on next frame
    m_lastControlSig = 0;
    m_lastGeomFrame = UINT64_MAX;
    m_lastGeomControlSig = 0;
    m_bplCount = 0;
    m_decodeResult = {};
    for (auto& inst : m_traceInstances) {
        if (inst.texId) {
            glDeleteTextures(1, &inst.texId);
            inst.texId = 0;
        }
    }
    m_traceInstances.clear();
    m_selectedTraceRow = -1;
}

void BitplanesView::SetOnJumpToMemory(std::function<void(uint32_t)> cb)
{
    m_onJumpToMemory = std::move(cb);
}

void BitplanesView::SetUserPreferences(core::UserPreferences* preferences)
{
    m_userPreferences = preferences;
    if (m_userPreferences)
        ApplyPreferences(m_userPreferences->Get().bitplanes);
}

core::UserPreferences::Preferences::BitplanePrefs BitplanesView::CurrentPreferences() const
{
    core::UserPreferences::Preferences::BitplanePrefs preferences;
    preferences.sourceMode = m_sourceMode;
    preferences.manualBplCount = m_manualBplCount;
    preferences.manualWidth = m_manualWidth;
    preferences.manualHeight = m_manualHeight;
    preferences.manualModulo1 = m_manualModulo1;
    preferences.manualModulo2 = m_manualModulo2;
    preferences.useManualModulo1 = m_useManualModulo1;
    preferences.useManualModulo2 = m_useManualModulo2;
    preferences.interleavedAuto = m_interleavedAuto;
    preferences.interleavedForce = m_interleavedForce;
    preferences.useRegisterPointers = m_useRegisterPointers;
    std::copy(std::begin(m_manualPointers), std::end(m_manualPointers),
              preferences.manualPointers.begin());
    preferences.useManualStride1 = m_useManualStride1;
    preferences.useManualStride2 = m_useManualStride2;
    preferences.manualStride1 = m_manualStride1;
    preferences.manualStride2 = m_manualStride2;
    preferences.overrideWidth = m_overrideWidth;
    preferences.overrideHeight = m_overrideHeight;
    preferences.applyPalette = m_applyPalette;
    preferences.showComposite = m_showComposite;
    preferences.zoom = m_zoom;
    preferences.lockAspect = m_lockAspect;
    preferences.fitToWindow = m_fitToWindow;
    preferences.showInspector = m_showInspector;
    return preferences;
}

void BitplanesView::ApplyPreferences(
    const core::UserPreferences::Preferences::BitplanePrefs& preferences)
{
    m_sourceMode = std::clamp(preferences.sourceMode, kSourceAuto, kSourceFrameBoundary);
    m_autoDetect = m_sourceMode != kSourceManual;
    m_traceMode = m_sourceMode == kSourceFrameTrace;
    m_manualBplCount = std::clamp(preferences.manualBplCount, 1, kMaxPlanes);
    m_manualWidth = std::clamp(preferences.manualWidth, 16, 2048);
    m_manualHeight = std::clamp(preferences.manualHeight, 1, 576);
    m_manualModulo1 = preferences.manualModulo1;
    m_manualModulo2 = preferences.manualModulo2;
    m_useManualModulo1 = preferences.useManualModulo1;
    m_useManualModulo2 = preferences.useManualModulo2;
    m_interleavedAuto = preferences.interleavedAuto;
    m_interleavedForce = preferences.interleavedForce;
    m_useRegisterPointers = preferences.useRegisterPointers;
    std::copy(preferences.manualPointers.begin(), preferences.manualPointers.end(),
              std::begin(m_manualPointers));
    m_useManualStride1 = preferences.useManualStride1;
    m_useManualStride2 = preferences.useManualStride2;
    m_manualStride1 = std::clamp(preferences.manualStride1, 1, 16384);
    m_manualStride2 = std::clamp(preferences.manualStride2, 1, 16384);
    m_overrideWidth = preferences.overrideWidth;
    m_overrideHeight = preferences.overrideHeight;
    m_applyPalette = preferences.applyPalette;
    m_showComposite = preferences.showComposite;
    m_zoom = std::clamp(preferences.zoom, 0.25f, 8.0f);
    m_lockAspect = preferences.lockAspect;
    m_fitToWindow = preferences.fitToWindow;
    m_showInspector = preferences.showInspector;
}

void BitplanesView::DrawPtrLink(const char* idSuffix, uint32_t addr)
{
    if (addr == 0) {
        ImGui::TextDisabled("---");
        return;
    }
    // Preferred path: the app-wide hyperlink (left-click jump + right-click actions menu).
    // Left-click defaults to the Memory Map window since bitplane pointers are best viewed there.
    if (m_addrCtx && m_addrCtx->valid()) {
        DrawAddressLink(idSuffix, addr, "$%06X", *m_addrCtx, AddressLinkHost::Window,
                        AddressDefault::MemoryMap);
        return;
    }
    // Fallback when no shared context is wired (e.g. standalone use): plain jump-to-memory link.
    if (m_onJumpToMemory) {
        ImGui::TextColored(kHyperlinkColor, "$%06X", addr);
        bool clicked = false;
        HoverableLink(&clicked);
        if (clicked)
            m_onJumpToMemory(addr);
        return;
    }
    ImGui::Text("$%06X", addr);
}

core::BitplaneLayout BitplanesView::GetBitplaneLayout()
{
    // Make sure geometry reflects the current frame even if the Bitplanes window is closed.
    EnsureGeometryDecoded();

    return m_decodeResult.layout;
}

// ---------------------------------------------------------------------------
// BuildTraceInstances
// ---------------------------------------------------------------------------
//
// Uses copper events as the primary split detector:
//   • BPLCON0 mode-bit changes  → pending split (absorbed if BPL1PTL follows ≤8 lines later)
//   • BPL1PTL write             → definitive split (uses pending vpos if pending is close)
// BplptEvents supply the pre-assembled 24-bit plane pointers (the copper writes H and L
// words separately; BplptEvent already assembles the full address at write time).

void BitplanesView::BuildTraceInstances(const adm::AmigaEmulator::BplptEvent* bplEvents,
                                        int bplEvtCount,
                                        const adm::AmigaEmulator::CopperExecEvent* copperEvents,
                                        int copperCount)
{
    m_traceInstances.clear();

    // Copper register offsets used for split detection
    constexpr uint16_t kBplcon0 = 0x100u;
    constexpr uint16_t kDdfstrt = 0x092u;
    constexpr uint16_t kDdfstop = 0x094u;
    // BPLxPTH = 0x0E0 + plane*4,  BPLxPTL = 0x0E2 + plane*4  (planes 0-7)
    constexpr uint16_t kBplPtBase = 0x0E0u;
    constexpr uint16_t kBplPtRange = 0x020u; // 8 planes × 4 bytes = 0x20

    // Mask of bits that define a "meaningful" screen-mode change in BPLCON0:
    //   bit15=HIRES, bits14-12=BPU, bit11=HAM, bit10=DBLPF, bit7=EHB, bit4=AGA-BPU
    constexpr uint16_t kModeMask = 0xFC90u;

    // -----------------------------------------------------------------------
    // Initialise live state from the first plane-0 BplptEvent (best accuracy),
    // falling back to end-of-frame hardware registers.
    // -----------------------------------------------------------------------
    uint16_t liveBplcon0 = m_regsValid ? m_regs.bplcon0 : 0;
    uint16_t liveDdfstrt = m_regsValid ? m_regs.ddfstrt : 0x0038u;
    uint16_t liveDdfstop = m_regsValid ? m_regs.ddfstop : 0x00D0u;

    for (int e = 0; e < bplEvtCount; e++) {
        if (bplEvents[e].plane == 0) {
            liveBplcon0 = bplEvents[e].bplcon0;
            liveDdfstrt = bplEvents[e].ddfstrt;
            liveDdfstop = bplEvents[e].ddfstop;
            break;
        }
    }

    // -----------------------------------------------------------------------
    // Phase 1: walk copper events and collect raw split points.
    // -----------------------------------------------------------------------
    struct RawSplit
    {
        uint16_t vpos;
        uint16_t bplcon0;
        uint16_t ddfstrt;
        uint16_t ddfstop;
    };
    std::vector<RawSplit> rawSplits;

    // A BPLCON0 mode change starts a "pending" split.  If BPL1PTL is written
    // within 8 scanlines we absorb the pending into one split (the BPLCON0 vpos
    // is used as vpos_start so the mode change takes effect from the right line).
    struct PendingSplit
    {
        bool valid = false;
        uint16_t vpos = 0;
        uint16_t bplcon0 = 0;
        uint16_t ddfstrt = 0;
        uint16_t ddfstop = 0;
    } pending;

    for (int i = 0; i < copperCount; i++) {
        const auto& cev = copperEvents[i];
        if (cev.word1 & 1u) // not a MOVE instruction
            continue;
        const uint16_t reg = cev.word1 & 0x01FEu;
        const uint16_t val = cev.word2;

        if (reg == kBplcon0) {
            if ((val & kModeMask) != (liveBplcon0 & kModeMask)) {
                // Meaningful mode change — flush any stale pending first
                if (pending.valid)
                    rawSplits.push_back(
                        {pending.vpos, pending.bplcon0, pending.ddfstrt, pending.ddfstop});
                pending = {true, cev.vpos, val, liveDdfstrt, liveDdfstop};
            }
            liveBplcon0 = val;
            if (pending.valid)
                pending.bplcon0 = val; // keep pending in sync with latest bplcon0
        } else if (reg == kDdfstrt) {
            liveDdfstrt = val;
            if (pending.valid)
                pending.ddfstrt = val;
        } else if (reg == kDdfstop) {
            liveDdfstop = val;
            if (pending.valid)
                pending.ddfstop = val;
        } else if (reg >= kBplPtBase && reg < kBplPtBase + kBplPtRange) {
            const int planeIdx = static_cast<int>((reg - kBplPtBase) / 4);
            const bool isHigh = ((reg - kBplPtBase) % 4) == 0;

            if (!isHigh && planeIdx == 0) {
                // BPL1PTL written — definitive split
                if (pending.valid &&
                    static_cast<int>(cev.vpos) - static_cast<int>(pending.vpos) <= 8) {
                    // Absorb: use the earlier BPLCON0-change vpos as the split point
                    rawSplits.push_back({pending.vpos, liveBplcon0, liveDdfstrt, liveDdfstop});
                    pending.valid = false;
                } else {
                    // Flush any stale pending first, then emit BPL1PTL split
                    if (pending.valid) {
                        rawSplits.push_back(
                            {pending.vpos, pending.bplcon0, pending.ddfstrt, pending.ddfstop});
                        pending.valid = false;
                    }
                    rawSplits.push_back({cev.vpos, liveBplcon0, liveDdfstrt, liveDdfstop});
                }
            }
        }
    }

    // Flush any remaining pending BPLCON0-only split (no subsequent BPL1PTL)
    if (pending.valid)
        rawSplits.push_back({pending.vpos, pending.bplcon0, pending.ddfstrt, pending.ddfstop});

    // Fall back to BplptEvent-based splits if copper gave us nothing
    if (rawSplits.empty()) {
        for (int e = 0; e < bplEvtCount; e++) {
            if (bplEvents[e].plane == 0)
                rawSplits.push_back({bplEvents[e].vpos, bplEvents[e].bplcon0, bplEvents[e].ddfstrt,
                                     bplEvents[e].ddfstop});
        }
    }

    if (rawSplits.empty())
        return;

    // Sort and deduplicate same-vpos entries (keep the last state — most complete)
    std::stable_sort(rawSplits.begin(), rawSplits.end(),
                     [](const RawSplit& a, const RawSplit& b) { return a.vpos < b.vpos; });
    {
        std::vector<RawSplit> deduped;
        for (const auto& rs : rawSplits) {
            if (!deduped.empty() && deduped.back().vpos == rs.vpos)
                deduped.back() = rs; // same scanline: overwrite with latest state
            else
                deduped.push_back(rs);
        }
        rawSplits = std::move(deduped);
    }

    // -----------------------------------------------------------------------
    // Phase 2: build BitplaneInstance for each split, assigning plane pointers
    // from BplptEvents.  For each instance [vpos_start, vpos_end), use the
    // first BplptEvent in that window for each plane (the pointer set at setup
    // time).  Pointers carry forward if not updated in the new window.
    // -----------------------------------------------------------------------

    // Helper: parse mode bits from bplcon0
    auto applyBplcon0 = [](BitplaneInstance& inst) {
        int bpu = (inst.bplcon0 >> 12) & 0x7;
        if (inst.bplcon0 & 0x0010)
            bpu |= 0x8; // AGA 8th plane bit
        inst.bplCount = std::min(bpu, 8);
        inst.resolution = core::ResolutionFromBplcon0(inst.bplcon0);
        inst.isHAM = (inst.bplcon0 >> 11) & 1;
        inst.isDblPF = (inst.bplcon0 >> 10) & 1;
    };

    // Current live pointers — carried forward across instances for planes that
    // aren't reloaded in a given section.
    uint32_t currentPtr[8] = {};

    for (size_t i = 0; i < rawSplits.size(); i++) {
        const auto& rs = rawSplits[i];
        const uint16_t endVpos = (i + 1 < rawSplits.size()) ? rawSplits[i + 1].vpos : 312u;

        // Look for BplptEvents inside [vpos_start, vpos_start + 16) for each plane.
        // We use a generous +16-line window so that copper setups spread over a few
        // lines are attributed to the instance that triggered them.
        const int windowEnd = static_cast<int>(rs.vpos) + 16;
        for (int e = 0; e < bplEvtCount; e++) {
            const auto& ev = bplEvents[e];
            if (ev.plane >= 8)
                continue;
            const int evVpos = static_cast<int>(ev.vpos);
            if (evVpos >= static_cast<int>(rs.vpos) && evVpos < windowEnd)
                currentPtr[ev.plane] = ev.ptr;
        }

        BitplaneInstance inst{};
        inst.vpos_start = rs.vpos;
        inst.vpos_end = endVpos;
        inst.bplcon0 = rs.bplcon0;
        inst.ddfstrt = rs.ddfstrt;
        inst.ddfstop = rs.ddfstop;
        for (int p = 0; p < 8; p++)
            inst.ptrs[p] = currentPtr[p];
        applyBplcon0(inst);
        m_traceInstances.push_back(inst);
    }
}

// ---------------------------------------------------------------------------
// AssignInstancePalettesFromCopper
// ---------------------------------------------------------------------------

void BitplanesView::AssignInstancePalettesFromCopper(
    const core::Custom& regs, const adm::AmigaEmulator::CopperExecEvent* copperEvents,
    int copperCount)
{
    if (m_traceInstances.empty())
        return;

    // Build a sorted list of (vpos_start, inst_idx) for the forward-pass
    struct InstRef
    {
        uint16_t vpos;
        int idx;
    };
    std::vector<InstRef> sorted;
    sorted.reserve(m_traceInstances.size());
    for (int i = 0; i < static_cast<int>(m_traceInstances.size()); i++)
        sorted.push_back({m_traceInstances[static_cast<size_t>(i)].vpos_start, i});
    std::sort(sorted.begin(), sorted.end(),
              [](const InstRef& a, const InstRef& b) { return a.vpos < b.vpos; });

    // Initialise live color state from end-of-frame hardware snapshot.
    // The copper replay will overwrite these as it walks forward through the frame.
    uint16_t liveColor[32];
    for (int c = 0; c < 32; c++)
        liveColor[c] = static_cast<uint16_t>(regs.color[c] & 0x0FFFu);

    int instPos = 0;
    int cevIdx = 0;
    const int sortedCount = static_cast<int>(sorted.size());

    while (instPos < sortedCount) {
        const uint16_t targetVpos = sorted[static_cast<size_t>(instPos)].vpos;

        // Consume all copper MOVE events up to and including targetVpos.
        // Palette writes typically appear on the same scanline as the BPL split
        // (the copper sets colours then re-arms the plane pointers / BPLCON0 in
        // one sequence), so we must include events at vpos == targetVpos before
        // snapshotting, otherwise those colour changes are missed.
        while (cevIdx < copperCount) {
            const auto& cev = copperEvents[cevIdx];
            if (cev.vpos > targetVpos)
                break;
            // MOVE instruction: bit 0 of word1 == 0
            if ((cev.word1 & 1u) == 0) {
                uint16_t reg = cev.word1 & 0x01FEu;
                if (reg >= kColor00Reg && reg <= kColor31Reg) {
                    int ci = static_cast<int>((reg - kColor00Reg) / 2); // 0..31
                    liveColor[ci] = cev.word2 & 0x0FFFu;
                }
            }
            ++cevIdx;
        }

        // Snapshot current liveColor into all instances at this vpos
        while (instPos < sortedCount && sorted[static_cast<size_t>(instPos)].vpos == targetVpos) {
            auto& inst =
                m_traceInstances[static_cast<size_t>(sorted[static_cast<size_t>(instPos)].idx)];
            inst.paletteValid = true;
            for (int c = 0; c < 32; c++)
                inst.palette[c] = OcsColorToImU32(liveColor[c]);
            ++instPos;
        }
    }
}

// ---------------------------------------------------------------------------
// BuildInstanceTexture
// ---------------------------------------------------------------------------

void BitplanesView::BuildInstanceTexture(
    int instIdx, const std::vector<adm::AmigaEmulator::MemoryRegion>& regions)
{
    if (instIdx < 0 || instIdx >= static_cast<int>(m_traceInstances.size()))
        return;
    auto& inst = m_traceInstances[static_cast<size_t>(instIdx)];

    int bplCount = std::min(inst.bplCount, kMaxPlanes);
    if (bplCount < 1)
        return;

    // Derive width from instance's DDF registers (shared formula, same as DecodeBitplanes)
    int instWidth = 320; // fallback
    {
        const int pw = core::FetchWidthPx(inst.ddfstrt, inst.ddfstop, inst.resolution);
        if (pw >= 16 && pw <= 1024)
            instWidth = pw;
    }

    // Use the actual scanline height for the instance.
    // Textures are scaled down in DrawTraceTable so very tall sections still display cleanly.
    int instHeight = static_cast<int>(inst.vpos_end) - static_cast<int>(inst.vpos_start);
    if (instHeight <= 0)
        instHeight = 1;
    if (instHeight > 576)
        instHeight = 576; // generous sanity cap (PAL/interlace max)

    int bytesPerRow = instWidth / 8;
    if (bytesPerRow == 0)
        return;

    // Use frame-global modulos (BPL1MOD/BPL2MOD don't typically change mid-frame)
    int mod1 = m_regsValid ? static_cast<int>(static_cast<int16_t>(m_regs.bpl1mod)) : 0;
    int mod2 = m_regsValid ? static_cast<int>(static_cast<int16_t>(m_regs.bpl2mod)) : 0;

    int totalPix = instWidth * instHeight;

    // Decode each plane into a scratch buffer (non-interleaved planar stride)
    std::vector<std::vector<uint8_t>> planeBits(static_cast<size_t>(bplCount));
    for (int p = 0; p < bplCount; p++) {
        planeBits[static_cast<size_t>(p)].assign(static_cast<size_t>(totalPix), 0);
        if (inst.ptrs[p] == 0)
            continue;
        int modulo = (p % 2 == 0) ? mod1 : mod2;
        int rowStride = bytesPerRow + modulo;
        for (int y = 0; y < instHeight; y++) {
            uint32_t rowAddr = inst.ptrs[p] + static_cast<uint32_t>(y * rowStride);
            DecodeRow(rowAddr, bytesPerRow,
                      planeBits[static_cast<size_t>(p)].data() + y * instWidth, regions);
        }
    }

    // Compose RGBA using per-instance palette
    int numColors = 1 << bplCount;
    m_pixelBuf.resize(static_cast<size_t>(totalPix * 4));

    for (int i = 0; i < totalPix; i++) {
        int ci = 0;
        for (int p = 0; p < bplCount; p++) {
            if (planeBits[static_cast<size_t>(p)][static_cast<size_t>(i)])
                ci |= (1 << p);
        }
        ci = std::min(ci, numColors - 1);

        ImU32 col;
        if (inst.paletteValid && ci < 32) {
            col = inst.palette[ci];
        } else {
            uint8_t v = (numColors > 1) ? static_cast<uint8_t>((ci * 255) / (numColors - 1)) : 0;
            col = IM_COL32(v, v, v, 255);
        }

        uint8_t* dst = m_pixelBuf.data() + i * 4;
        dst[0] = static_cast<uint8_t>((col >> IM_COL32_R_SHIFT) & 0xFF);
        dst[1] = static_cast<uint8_t>((col >> IM_COL32_G_SHIFT) & 0xFF);
        dst[2] = static_cast<uint8_t>((col >> IM_COL32_B_SHIFT) & 0xFF);
        dst[3] = 255;
    }

    inst.texId = UploadRGBATexture(inst.texId, instWidth, instHeight, m_pixelBuf.data(), inst.texW,
                                   inst.texH);
    inst.texW = instWidth;
    inst.texH = instHeight;
}

// ---------------------------------------------------------------------------
// DrawTraceTable
// ---------------------------------------------------------------------------

void BitplanesView::DrawTraceTable()
{
    if (m_traceInstances.empty()) {
        ImGui::TextDisabled("No bitplane instances detected this frame.");
        ImGui::TextDisabled("BplptEvent capture may not be active.");
        return;
    }

    ImGui::TextDisabled("%d display region%s", static_cast<int>(m_traceInstances.size()),
                        m_traceInstances.size() == 1 ? "" : "s");
    ImGui::SameLine();
    ImGui::TextDisabled("· select a region to inspect it");

    ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                            kAdjustableColumns;

    if (!ImGui::BeginTable("bpl_trace_tbl", 4, flags))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Scanlines", ImGuiTableColumnFlags_WidthFixed, 76.0f);
    ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 86.0f);
    ImGui::TableSetupColumn("Region preview", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("BPL1PT", ImGuiTableColumnFlags_WidthFixed, 76.0f);
    ImGui::TableHeadersRow();

    const float rowHeight = 76.0f;

    for (int row = 0; row < static_cast<int>(m_traceInstances.size()); row++) {
        const auto& inst = m_traceInstances[static_cast<size_t>(row)];

        ImGui::PushID(row);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);

        ImGui::TableSetColumnIndex(0);
        bool selected = (m_selectedTraceRow == row);
        char scanlineLabel[32];
        snprintf(scanlineLabel, sizeof(scanlineLabel), "%d–%d", static_cast<int>(inst.vpos_start),
                 static_cast<int>(inst.vpos_end));
        const ImVec2 cellStart = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##region", selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, rowHeight - 4.0f)))
            m_selectedTraceRow = row;
        ImGui::SetCursorScreenPos(cellStart);
        ImGui::TextUnformatted(scanlineLabel);
        ImGui::TextDisabled("%d lines", std::max(0, static_cast<int>(inst.vpos_end) -
                                                        static_cast<int>(inst.vpos_start)));

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(core::ResolutionLabel(inst.resolution));
        ImGui::TextDisabled("%d plane%s", inst.bplCount, inst.bplCount == 1 ? "" : "s");
        if (inst.isHAM)
            ImGui::TextDisabled("HAM");
        else if (inst.isDblPF)
            ImGui::TextDisabled("Dual PF");

        ImGui::TableSetColumnIndex(2);
        if (inst.texId && inst.texW > 0 && inst.texH > 0) {
            const float maxWidth = std::max(24.0f, ImGui::GetContentRegionAvail().x - 4.0f);
            const float maxHeight = rowHeight - 8.0f;
            const float scale = std::min(maxWidth / static_cast<float>(inst.texW),
                                         maxHeight / static_cast<float>(inst.texH));
            const ImVec2 size(std::max(1.0f, static_cast<float>(inst.texW) * scale),
                              std::max(1.0f, static_cast<float>(inst.texH) * scale));
            const float verticalPadding = std::max(0.0f, (maxHeight - size.y) * 0.5f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + verticalPadding);
            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
            ImGui::Image((ImTextureID)(uintptr_t)inst.texId, size);
            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
        } else {
            ImGui::TextDisabled("No preview data");
        }

        ImGui::TableSetColumnIndex(3);
        char suffix[24];
        snprintf(suffix, sizeof(suffix), "row%d", row);
        DrawPtrLink(suffix, inst.ptrs[0]);

        ImGui::PopID();
    }

    ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// DrawTraceExpandedRow
// ---------------------------------------------------------------------------

void BitplanesView::DrawTraceExpandedRow(int instIdx)
{
    if (instIdx < 0 || instIdx >= static_cast<int>(m_traceInstances.size()))
        return;
    const auto& inst = m_traceInstances[static_cast<size_t>(instIdx)];

    const int lineCount =
        std::max(0, static_cast<int>(inst.vpos_end) - static_cast<int>(inst.vpos_start));
    ImGui::TextColored(ImVec4(0.95f, 0.82f, 0.35f, 1.0f), "Region %d", instIdx + 1);
    ImGui::SameLine();
    ImGui::TextDisabled("scanlines %d–%d · %d lines", static_cast<int>(inst.vpos_start),
                        static_cast<int>(inst.vpos_end), lineCount);

    ImGui::SeparatorText("Preview");
    if (inst.texId && inst.texW > 0 && inst.texH > 0) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float previewHeight = std::clamp(available.y * 0.42f, 80.0f, 300.0f);
        const float scale = std::min({available.x / static_cast<float>(inst.texW),
                                      previewHeight / static_cast<float>(inst.texH), 4.0f});
        const float imgW = static_cast<float>(inst.texW) * scale;
        const float imgH = static_cast<float>(inst.texH) * scale;

        if (available.x > imgW)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available.x - imgW) * 0.5f);

        ImDrawList* dlP = ImGui::GetWindowDrawList();
        ImVec2 imgP = ImGui::GetCursorScreenPos();
        dlP->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
        ImGui::Image((ImTextureID)(uintptr_t)inst.texId, ImVec2(imgW, imgH));
        dlP->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
        dlP->AddRect(imgP, ImVec2(imgP.x + imgW, imgP.y + imgH), IM_COL32(160, 160, 160, 200), 0.0f,
                     0, 1.0f);
    } else {
        ImGui::TextDisabled("No preview data is available for this region.");
    }

    ImGui::SeparatorText("Display state");
    if (ImGui::BeginTable("##trace_state", 2,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Resolution");
        ImGui::TextUnformatted(core::ResolutionLabel(inst.resolution));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Planes / mode");
        ImGui::Text("%d plane%s%s%s", inst.bplCount, inst.bplCount == 1 ? "" : "s",
                    inst.isHAM ? " · HAM" : "", inst.isDblPF ? " · Dual playfield" : "");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("BPLCON0");
        ImGui::Text("$%04X", inst.bplcon0);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("DDF fetch");
        ImGui::Text("$%04X – $%04X", static_cast<int>(inst.ddfstrt),
                    static_cast<int>(inst.ddfstop));
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Plane pointers");
    if (ImGui::BeginTable("##trace_ptrs", 2, ImGuiTableFlags_SizingStretchSame)) {
        const int showPlanes = std::max(inst.bplCount, 1);
        for (int plane = 0; plane < showPlanes; ++plane) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("BPL%dPT", plane + 1);
            ImGui::SameLine(0.0f, 6.0f);
            char suffix[24];
            snprintf(suffix, sizeof(suffix), "trace%d_%d", instIdx, plane);
            DrawPtrLink(suffix, inst.ptrs[plane]);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Palette");
    if (!inst.paletteValid) {
        ImGui::TextDisabled("No palette data is available for this region.");
        return;
    }

    const int numColors = std::min(1 << inst.bplCount, 32);
    const float swatchWidth = 24.0f;
    const float swatchHeight = 20.0f;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const int perRow = std::clamp(
        static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (swatchWidth + gap)), 1, 16);
    for (int color = 0; color < numColors; ++color) {
        char id[24];
        snprintf(id, sizeof(id), "##tracecolor%d", color);
        ImGui::ColorButton(id, ImGui::ColorConvertU32ToFloat4(inst.palette[color]),
                           ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip,
                           ImVec2(swatchWidth, swatchHeight));
        if (ImGui::IsItemHovered()) {
            const ImU32 value = inst.palette[color];
            const uint8_t r = static_cast<uint8_t>((value >> IM_COL32_R_SHIFT) & 0xFF);
            const uint8_t g = static_cast<uint8_t>((value >> IM_COL32_G_SHIFT) & 0xFF);
            const uint8_t b = static_cast<uint8_t>((value >> IM_COL32_B_SHIFT) & 0xFF);
            const uint16_t ocs =
                static_cast<uint16_t>(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
            ImGui::SetTooltip("COLOR%02d  $%03X", color, static_cast<int>(ocs));
        }
        if ((color + 1) % perRow != 0 && color + 1 < numColors)
            ImGui::SameLine();
    }
}

// ----- decode ----------------------------------------------------------------

// Per-frame decode gate
// ---------------------------------------------------------------------------

uint64_t BitplanesView::ControlSignature() const
{
    uint64_t sig = 1469598103934665603ull; // FNV-1a offset basis
    auto mix = [&](uint64_t v) { sig = (sig ^ (v + 0x9e3779b97f4a7c15ull)) * 1099511628211ull; };
    mix(m_autoDetect);
    mix(static_cast<uint32_t>(m_manualBplCount));
    mix(static_cast<uint32_t>(m_manualWidth));
    mix(static_cast<uint32_t>(m_manualHeight));
    mix(m_useManualModulo1);
    mix(static_cast<uint32_t>(m_manualModulo1));
    mix(m_useManualModulo2);
    mix(static_cast<uint32_t>(m_manualModulo2));
    mix(m_interleavedAuto);
    mix(m_interleavedForce);
    mix(m_overrideWidth);
    mix(m_overrideHeight);
    mix(m_applyPalette);
    mix(m_showComposite);
    mix(m_traceMode);
    mix(static_cast<uint32_t>(m_sourceMode));
    mix(static_cast<uint32_t>(m_autoTraceRegion));
    mix(m_useRegisterPointers);
    for (uint32_t pointer : m_manualPointers)
        mix(pointer);
    mix(m_useManualStride1);
    mix(m_useManualStride2);
    mix(static_cast<uint32_t>(m_manualStride1));
    mix(static_cast<uint32_t>(m_manualStride2));
    return sig;
}

void BitplanesView::EnsureGeometryDecoded()
{
    // Texture-free geometry decode, frame-gated independently of the full decode so it is
    // cheap to call every frame (e.g. for the blit overlay) regardless of window visibility.
    uint64_t emuFrame = m_emulatorView ? m_emulatorView->GetEmulatorFrameCount() : 0;
    uint64_t controlSig = ControlSignature();
    if (emuFrame == m_lastDecodeEmuFrame && controlSig == m_lastControlSig && m_regsValid)
        return; // the full result is newer and contains the same geometry
    if (emuFrame == m_lastGeomFrame && controlSig == m_lastGeomControlSig)
        return;
    m_lastGeomFrame = emuFrame;
    m_lastGeomControlSig = controlSig;

    m_regsValid = false;
    if (!m_emulatorView || !m_emulatorView->IsRunning())
        return;

    core::Custom regs{};
    if (!m_emulatorView->GetEmulator()->GetCustomRegisters(regs))
        return;

    m_regs = regs;
    m_regsValid = true;
    ResolveGeometry();
}

void BitplanesView::EnsureDecoded()
{
    // Decode is expensive (full plane decode + texture uploads). Only redo it when the emulator
    // advanced a frame or a decode-affecting control changed; while paused with stable controls
    // the cached textures are still valid, so we keep them on screen for free.
    uint64_t emuFrame = m_emulatorView ? m_emulatorView->GetEmulatorFrameCount() : 0;
    uint64_t controlSig = ControlSignature();
    if (emuFrame == m_lastDecodeEmuFrame && controlSig == m_lastControlSig)
        return;
    m_lastDecodeEmuFrame = emuFrame;
    m_lastControlSig = controlSig;

    m_regsValid = false;
    if (!m_emulatorView || !m_emulatorView->IsRunning())
        return;

    core::Custom regs{};
    if (!m_emulatorView->GetEmulator()->GetCustomRegisters(regs))
        return;

    m_regs = regs;
    m_regsValid = true;

    auto regions = m_emulatorView->GetFreshMemoryRegions();

    m_chipRamHostPtr = nullptr;
    m_chipRamAmigaBase = 0;
    m_chipRamSize = 0;
    for (const auto& r : regions) {
        if (r.hostPtr && r.amigaAddr == 0x000000) {
            m_chipRamHostPtr = r.hostPtr;
            m_chipRamAmigaBase = r.amigaAddr;
            m_chipRamSize = r.size;
            break;
        }
    }

    DecodeBitplanes(regions);
    m_autoUsingTrace = false;

    const bool needsAutoFallback = m_sourceMode == kSourceAuto && m_bplCount == 0;
    if (m_traceMode || needsAutoFallback) {
        RebuildTraceInstances(regions, m_traceMode);
        if (needsAutoFallback)
            DecodeAutoTraceRegion(regions);
    }
}

// ---------------------------------------------------------------------------
// Decode registers → resolve geometry and build textures
// ---------------------------------------------------------------------------

core::BitplaneDecodeOptions BitplanesView::DecodeOptions() const
{
    core::BitplaneDecodeOptions options;
    options.automatic = m_autoDetect;
    options.manualPlaneCount = m_manualBplCount;
    options.manualWidth = m_manualWidth;
    options.manualHeight = m_manualHeight;
    options.overrideWidth = m_overrideWidth;
    options.overrideHeight = m_overrideHeight;
    options.overrideModulo1 = m_useManualModulo1;
    options.overrideModulo2 = m_useManualModulo2;
    options.manualModulo1 = m_manualModulo1;
    options.manualModulo2 = m_manualModulo2;
    options.automaticInterleaving = m_interleavedAuto;
    options.forcedInterleaving = m_interleavedForce;
    options.useRegisterPointers = m_useRegisterPointers;
    for (int plane = 0; plane < kMaxPlanes; ++plane)
        options.manualPointers[static_cast<size_t>(plane)] = m_manualPointers[plane];
    options.overrideStride1 = m_useManualStride1;
    options.overrideStride2 = m_useManualStride2;
    options.manualStride1 = m_manualStride1;
    options.manualStride2 = m_manualStride2;
    return options;
}

void BitplanesView::ApplyDecodedLayout(const core::BitplaneLayout& layout)
{
    m_bplCount = layout.bplCount;
    m_screenWidth = layout.screenWidth;
    m_screenHeight = layout.screenHeight;
    m_modulo1 = layout.modulo1;
    m_modulo2 = layout.modulo2;
    m_interleaved = layout.interleaved;
    for (int plane = 0; plane < kMaxPlanes; ++plane)
        m_bplBase[plane] = layout.bplBase[plane];
}

void BitplanesView::ResolveGeometry()
{
    m_decodeResult = core::BitplaneDecoder::Decode(m_regs, DecodeOptions(), {}, false);
    ApplyDecodedLayout(m_decodeResult.layout);
}

void BitplanesView::DecodeBitplanes(const std::vector<adm::AmigaEmulator::MemoryRegion>& regions)
{
    std::vector<core::BitplaneMemoryView> memory;
    memory.reserve(regions.size());
    for (const auto& region : regions)
        memory.push_back({region.hostPtr, region.amigaAddr, region.size});

    m_previewPaletteOverride = false;
    m_decodeResult = core::BitplaneDecoder::Decode(m_regs, DecodeOptions(), memory);
    ApplyDecodeResult(regions);
}

void BitplanesView::ApplyDecodeResult(const std::vector<adm::AmigaEmulator::MemoryRegion>& regions)
{
    ApplyDecodedLayout(m_decodeResult.layout);
    for (int plane = 0; plane < kMaxPlanes; ++plane)
        m_decodedBits[plane] = m_decodeResult.planeBits[static_cast<size_t>(plane)];

    // A missing pointer must invalidate any texture retained from an earlier frame.
    for (int plane = 0; plane < kMaxPlanes; ++plane) {
        if (plane >= m_bplCount || m_bplBase[plane] == 0 || m_decodedBits[plane].empty()) {
            if (m_planeTex[plane]) {
                glDeleteTextures(1, &m_planeTex[plane]);
                m_planeTex[plane] = 0;
            }
            m_planeTexW[plane] = 0;
            m_planeTexH[plane] = 0;
        }
    }

    // --- Build textures from cached decoded bits ---
    for (int i = 0; i < m_bplCount; i++)
        BuildPlaneTexture(i, m_screenWidth, m_screenHeight, regions);

    if (m_showComposite && m_bplCount > 0)
        BuildCompositeTexture(m_bplCount, m_screenWidth, m_screenHeight, regions);
    else if (m_compositeTex) {
        glDeleteTextures(1, &m_compositeTex);
        m_compositeTex = 0;
        m_compositeTexW = 0;
        m_compositeTexH = 0;
        m_compositeColorIdx.clear();
    }
}

void BitplanesView::RebuildTraceInstances(
    const std::vector<adm::AmigaEmulator::MemoryRegion>& regions, bool buildTextures)
{
    for (auto& inst : m_traceInstances) {
        if (inst.texId) {
            glDeleteTextures(1, &inst.texId);
            inst.texId = 0;
        }
    }
    m_traceInstances.clear();

    const int maxCopperEvents = adm::AmigaEmulator::kMaxCopperExecEvents;
    m_copperEventBuf.resize(static_cast<size_t>(maxCopperEvents));
    const int copperCount = m_emulatorView->GetEmulator()->GetCopperExecEvents(
        m_copperEventBuf.data(), maxCopperEvents);

    const int maxBplEvents = adm::AmigaEmulator::kMaxBplptEvents;
    m_bplptEventBuf.resize(static_cast<size_t>(maxBplEvents));
    const int bplEventCount = m_emulatorView->GetBplptEvents(m_bplptEventBuf.data(), maxBplEvents);

    BuildTraceInstances(m_bplptEventBuf.data(), bplEventCount, m_copperEventBuf.data(),
                        copperCount);
    AssignInstancePalettesFromCopper(m_regs, m_copperEventBuf.data(), copperCount);

    if (buildTextures) {
        for (int index = 0; index < static_cast<int>(m_traceInstances.size()); ++index)
            BuildInstanceTexture(index, regions);
    }
}

bool BitplanesView::IsUsableTraceRegion(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_traceInstances.size()))
        return false;
    const auto& instance = m_traceInstances[static_cast<size_t>(index)];
    if (instance.bplCount <= 0 || instance.vpos_end <= instance.vpos_start)
        return false;
    for (int plane = 0; plane < std::min(instance.bplCount, kMaxPlanes); ++plane) {
        if (instance.ptrs[plane] != 0)
            return true;
    }
    return false;
}

int BitplanesView::BestTraceRegion() const
{
    int bestIndex = -1;
    int bestLines = -1;
    int bestPlanes = -1;
    for (int index = 0; index < static_cast<int>(m_traceInstances.size()); ++index) {
        if (!IsUsableTraceRegion(index))
            continue;
        const auto& instance = m_traceInstances[static_cast<size_t>(index)];
        const int lines = static_cast<int>(instance.vpos_end) - instance.vpos_start;
        if (lines > bestLines || (lines == bestLines && instance.bplCount > bestPlanes)) {
            bestIndex = index;
            bestLines = lines;
            bestPlanes = instance.bplCount;
        }
    }
    return bestIndex;
}

int BitplanesView::AdjacentTraceRegion(int direction) const
{
    const int count = static_cast<int>(m_traceInstances.size());
    if (count == 0)
        return -1;
    const int start =
        IsUsableTraceRegion(m_autoTraceRegion) ? m_autoTraceRegion : BestTraceRegion();
    if (start < 0)
        return -1;
    for (int step = 1; step <= count; ++step) {
        const int candidate = (start + direction * step % count + count) % count;
        if (IsUsableTraceRegion(candidate))
            return candidate;
    }
    return start;
}

bool BitplanesView::DecodeAutoTraceRegion(
    const std::vector<adm::AmigaEmulator::MemoryRegion>& regions)
{
    if (!IsUsableTraceRegion(m_autoTraceRegion))
        m_autoTraceRegion = BestTraceRegion();
    if (!IsUsableTraceRegion(m_autoTraceRegion)) {
        m_decodeResult.diagnostics.push_back({core::BitplaneDiagnosticSeverity::Warning,
                                              "The frame boundary has no active bitplanes and no "
                                              "decodable trace region was found."});
        return false;
    }

    const auto& instance = m_traceInstances[static_cast<size_t>(m_autoTraceRegion)];
    int width = 320;
    {
        const int detectedWidth =
            core::FetchWidthPx(instance.ddfstrt, instance.ddfstop, instance.resolution);
        if (detectedWidth >= 16 && detectedWidth <= 2048)
            width = detectedWidth;
    }
    const int height = std::clamp(
        static_cast<int>(instance.vpos_end) - static_cast<int>(instance.vpos_start), 1, 576);

    core::Custom traceRegisters = m_regs;
    traceRegisters.bplcon0 = instance.bplcon0;
    traceRegisters.ddfstrt = instance.ddfstrt;
    traceRegisters.ddfstop = instance.ddfstop;
    traceRegisters.dmaconr |= 0x0100;

    core::BitplaneDecodeOptions options = DecodeOptions();
    options.automatic = false;
    options.manualPlaneCount = std::clamp(instance.bplCount, 1, kMaxPlanes);
    options.manualWidth = m_overrideWidth ? m_manualWidth : width;
    options.manualHeight = m_overrideHeight ? m_manualHeight : height;
    options.useRegisterPointers = false;
    for (int plane = 0; plane < kMaxPlanes; ++plane)
        options.manualPointers[static_cast<size_t>(plane)] = instance.ptrs[plane];

    std::vector<core::BitplaneMemoryView> memory;
    memory.reserve(regions.size());
    for (const auto& region : regions)
        memory.push_back({region.hostPtr, region.amigaAddr, region.size});

    m_decodeResult = core::BitplaneDecoder::Decode(traceRegisters, options, memory);
    m_decodeResult.layout.diwVStart = instance.vpos_start;
    m_decodeResult.diagnostics.insert(
        m_decodeResult.diagnostics.begin(),
        {core::BitplaneDiagnosticSeverity::Info,
         "The frame-boundary snapshot has no active bitplanes; Auto is showing a captured trace "
         "region instead."});

    m_previewPaletteOverride = instance.paletteValid;
    if (instance.paletteValid) {
        for (int color = 0; color < 32; ++color) {
            const ImU32 value = instance.palette[color];
            const uint8_t r = static_cast<uint8_t>((value >> IM_COL32_R_SHIFT) & 0xFF);
            const uint8_t g = static_cast<uint8_t>((value >> IM_COL32_G_SHIFT) & 0xFF);
            const uint8_t b = static_cast<uint8_t>((value >> IM_COL32_B_SHIFT) & 0xFF);
            m_previewPalette[color] =
                static_cast<uint16_t>(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
        }
    }

    m_autoUsingTrace = true;
    ApplyDecodeResult(regions);
    return true;
}

uint16_t BitplanesView::PreviewColor(int index) const
{
    index = std::clamp(index, 0, 31);
    return m_previewPaletteOverride ? m_previewPalette[index] : m_regs.color[index];
}

// ---------------------------------------------------------------------------
// DecodeRow: read bytesPerRow bytes from chip RAM at rowAddr, unpack to pixels
// ---------------------------------------------------------------------------

void BitplanesView::DecodeRow(uint32_t rowAddr, int bytesPerRow, uint8_t* pixelRow,
                              const std::vector<adm::AmigaEmulator::MemoryRegion>& regions) const
{
    if (m_chipRamHostPtr && rowAddr >= m_chipRamAmigaBase &&
        (rowAddr + (uint32_t)bytesPerRow) <= (m_chipRamAmigaBase + m_chipRamSize)) {
        const uint8_t* src = m_chipRamHostPtr + (rowAddr - m_chipRamAmigaBase);
        for (int b = 0; b < bytesPerRow; b++) {
            uint8_t byte = src[b];
            uint8_t* dst = pixelRow + b * 8;
            dst[0] = (byte >> 7) & 1;
            dst[1] = (byte >> 6) & 1;
            dst[2] = (byte >> 5) & 1;
            dst[3] = (byte >> 4) & 1;
            dst[4] = (byte >> 3) & 1;
            dst[5] = (byte >> 2) & 1;
            dst[6] = (byte >> 1) & 1;
            dst[7] = (byte >> 0) & 1;
        }
        return;
    }
    // Slow-path fallback for non-chip-RAM addresses
    for (int b = 0; b < bytesPerRow; b++) {
        uint8_t byte = 0;
        ReadChipByte(rowAddr + b, byte, regions);
        for (int bit = 7; bit >= 0; bit--)
            pixelRow[b * 8 + (7 - bit)] = (byte >> bit) & 1;
    }
}

// ---------------------------------------------------------------------------
// BuildPlaneTexture
// ---------------------------------------------------------------------------

void BitplanesView::BuildPlaneTexture(
    int planeIdx, int width, int height,
    const std::vector<adm::AmigaEmulator::MemoryRegion>& /*regions*/)
{
    if (width <= 0 || height <= 0)
        return;
    if (m_bplBase[planeIdx] == 0 || m_decodedBits[planeIdx].empty())
        return;

    // Build palette entry (white for set bits, black for clear)
    // If palette is applied, colour[1] is used for this plane's "on" colour.
    ImU32 colOn = IM_COL32(255, 255, 255, 255);
    ImU32 colOff = IM_COL32(0, 0, 0, 255);

    if (m_applyPalette && m_regsValid) {
        // Use colour register corresponding to this plane's contribution.
        // Simple scheme: colour index = bit mask of just this plane = 2^planeIdx
        // That maps to register index 2^planeIdx (1, 2, 4, 8, ...)
        int ci = 1 << planeIdx;
        if (ci < 32)
            colOn = OcsColorToImU32(PreviewColor(ci));
        colOff = IM_COL32(0, 0, 0, 255);
    }

    uint8_t rOn = (colOn >> IM_COL32_R_SHIFT) & 0xFF;
    uint8_t gOn = (colOn >> IM_COL32_G_SHIFT) & 0xFF;
    uint8_t bOn = (colOn >> IM_COL32_B_SHIFT) & 0xFF;
    uint8_t rOff = (colOff >> IM_COL32_R_SHIFT) & 0xFF;
    uint8_t gOff = (colOff >> IM_COL32_G_SHIFT) & 0xFF;
    uint8_t bOff = (colOff >> IM_COL32_B_SHIFT) & 0xFF;

    int totalPix = width * height;
    m_pixelBuf.resize(totalPix * 4);
    const uint8_t* bits = m_decodedBits[planeIdx].data();
    for (int i = 0; i < totalPix; i++) {
        uint8_t* dst = m_pixelBuf.data() + i * 4;
        if (bits[i]) {
            dst[0] = rOn;
            dst[1] = gOn;
            dst[2] = bOn;
            dst[3] = 255;
        } else {
            dst[0] = rOff;
            dst[1] = gOff;
            dst[2] = bOff;
            dst[3] = 255;
        }
    }

    m_planeTex[planeIdx] = UploadRGBATexture(m_planeTex[planeIdx], width, height, m_pixelBuf.data(),
                                             m_planeTexW[planeIdx], m_planeTexH[planeIdx]);
    m_planeTexW[planeIdx] = width;
    m_planeTexH[planeIdx] = height;
}

// ---------------------------------------------------------------------------
// BuildCompositeTexture
// ---------------------------------------------------------------------------

void BitplanesView::BuildCompositeTexture(
    int bplCount, int width, int height,
    const std::vector<adm::AmigaEmulator::MemoryRegion>& /*regions*/)
{
    if (bplCount <= 0 || width <= 0 || height <= 0)
        return;

    int numColors = 1 << bplCount; // 2, 4, 8, …

    // Build colour LUT from Amiga palette (or greyscale). EHB indices 32-63 use the first
    // 32 colour registers at half intensity.
    std::vector<ImU32> lut(numColors);
    for (int i = 0; i < numColors; i++) {
        if (m_applyPalette && m_regsValid && i < 32) {
            lut[i] = OcsColorToImU32(PreviewColor(i));
        } else if (m_applyPalette && m_regsValid && i >= 32 && i < 64 &&
                   m_decodeResult.layout.displayMode == core::BitplaneDisplayMode::ExtraHalfBrite) {
            const ImU32 base = OcsColorToImU32(PreviewColor(i - 32));
            const uint8_t r = static_cast<uint8_t>(((base >> IM_COL32_R_SHIFT) & 0xFF) / 2);
            const uint8_t g = static_cast<uint8_t>(((base >> IM_COL32_G_SHIFT) & 0xFF) / 2);
            const uint8_t b = static_cast<uint8_t>(((base >> IM_COL32_B_SHIFT) & 0xFF) / 2);
            lut[i] = IM_COL32(r, g, b, 255);
        } else {
            // Greyscale ramp: 0 = black, numColors-1 = white
            uint8_t v = static_cast<uint8_t>((i * 255) / std::max(1, numColors - 1));
            lut[i] = IM_COL32(v, v, v, 255);
        }
    }
    // Ensure index 0 is always black
    if (!m_applyPalette)
        lut[0] = IM_COL32(0, 0, 0, 255);

    int totalPix = width * height;
    m_pixelBuf.resize(totalPix * 4);
    m_compositeColorIdx.resize(totalPix);

    m_compositeColorIdx = m_decodeResult.colorIndices;
    if (m_compositeColorIdx.size() != static_cast<size_t>(totalPix))
        m_compositeColorIdx.assign(static_cast<size_t>(totalPix), 0);

    // Pass 2: LUT → RGBA
    for (int i = 0; i < totalPix; i++) {
        int ci = std::min((int)m_compositeColorIdx[i], numColors - 1);
        ImU32 col = lut[ci];
        uint8_t* dst = m_pixelBuf.data() + i * 4;
        dst[0] = (col >> IM_COL32_R_SHIFT) & 0xFF;
        dst[1] = (col >> IM_COL32_G_SHIFT) & 0xFF;
        dst[2] = (col >> IM_COL32_B_SHIFT) & 0xFF;
        dst[3] = 255;
    }

    m_compositeTex = UploadRGBATexture(m_compositeTex, width, height, m_pixelBuf.data(),
                                       m_compositeTexW, m_compositeTexH);
    m_compositeTexW = width;
    m_compositeTexH = height;
}

// ---------------------------------------------------------------------------
// ReadChipByte
// ---------------------------------------------------------------------------

bool BitplanesView::ReadChipByte(uint32_t addr, uint8_t& out,
                                 const std::vector<adm::AmigaEmulator::MemoryRegion>& regions) const
{
    for (const auto& r : regions) {
        if (!r.hostPtr)
            continue;
        if (addr >= r.amigaAddr && addr < r.amigaAddr + r.size) {
            out = r.hostPtr[addr - r.amigaAddr];
            return true;
        }
    }
    out = 0;
    return false;
}

// ---------------------------------------------------------------------------
// DrawControls
// ---------------------------------------------------------------------------

void BitplanesView::DrawToolbar()
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Source");
    ImGui::SameLine();

    constexpr int sourceValues[] = {kSourceAuto, kSourceFrameBoundary, kSourceFrameTrace,
                                    kSourceManual};
    const char* labels[] = {"Auto", "Frame boundary", "Frame trace", "Manual"};
    for (int button = 0; button < 4; ++button) {
        if (button > 0)
            ImGui::SameLine(0.0f, 3.0f);
        const int source = sourceValues[button];
        if (ImGui::RadioButton(labels[button], m_sourceMode == source)) {
            m_sourceMode = source;
            m_autoDetect = source != kSourceManual;
            m_traceMode = source == kSourceFrameTrace;
            m_selectedTraceRow = -1;
        }
    }

    if (!m_traceMode) {
        ImGui::Spacing();
        ImGui::Checkbox("Composite", &m_showComposite);
        ImGui::SameLine();
        ImGui::Checkbox("Palette", &m_applyPalette);
        ImGui::SameLine(0.0f, 18.0f);
        ImGui::Checkbox("Fit", &m_fitToWindow);
        if (!m_fitToWindow) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("##bplzoom", &m_zoom, 0.25f, 8.0f, "%.2fx");
        }
        ImGui::SameLine();
        ImGui::Checkbox("1:1 pixels", &m_lockAspect);
    }

    if (!m_traceMode) {
        ImGui::SameLine();
        const char* inspectorLabel = m_showInspector ? "Hide inspector" : "Show inspector";
        if (ImGui::SmallButton(inspectorLabel))
            m_showInspector = !m_showInspector;
    }
}

void BitplanesView::DrawStatusBar()
{
    if (!m_regsValid) {
        ImGui::TextDisabled("No live custom-register snapshot is available.");
        return;
    }

    if (m_traceMode) {
        ImGui::Text("frame trace  |  %d display region%s captured",
                    static_cast<int>(m_traceInstances.size()),
                    m_traceInstances.size() == 1 ? "" : "s");
        return;
    }

    const auto& layout = m_decodeResult.layout;
    const char* resolution = "LORES";
    if (layout.resolution == core::BitplaneResolution::Hires)
        resolution = "HIRES";
    else if (layout.resolution == core::BitplaneResolution::SuperHires)
        resolution = "SUPERHIRES";

    const char* mode = "indexed";
    switch (layout.displayMode) {
        case core::BitplaneDisplayMode::ExtraHalfBrite:
            mode = "EHB";
            break;
        case core::BitplaneDisplayMode::DualPlayfield:
            mode = "dual playfield";
            break;
        case core::BitplaneDisplayMode::Ham6:
            mode = "HAM6";
            break;
        case core::BitplaneDisplayMode::Ham8:
            mode = "HAM8";
            break;
        default:
            break;
    }

    const char* source = "auto · frame boundary";
    if (m_sourceMode == kSourceManual)
        source = "manual";
    else if (m_sourceMode == kSourceFrameBoundary)
        source = "frame boundary";
    else if (m_sourceMode == kSourceAuto && m_autoUsingTrace)
        source = "auto · trace fallback";
    ImGui::Text("%s  |  %d plane%s  |  %s %dx%d  |  %s  |  %s", source, layout.bplCount,
                layout.bplCount == 1 ? "" : "s", resolution, layout.screenWidth,
                layout.screenHeight, layout.interleaved ? "interleaved" : "planar", mode);

    if (m_sourceMode == kSourceAuto && m_autoUsingTrace && IsUsableTraceRegion(m_autoTraceRegion)) {
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::PushID("auto_trace_navigation");
        if (ImGui::ArrowButton("previous", ImGuiDir_Left))
            m_autoTraceRegion = AdjacentTraceRegion(-1);
        const auto& instance = m_traceInstances[static_cast<size_t>(m_autoTraceRegion)];
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::Text("region %d/%d · lines %d–%d", m_autoTraceRegion + 1,
                    static_cast<int>(m_traceInstances.size()),
                    static_cast<int>(instance.vpos_start), static_cast<int>(instance.vpos_end));
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::ArrowButton("next", ImGuiDir_Right))
            m_autoTraceRegion = AdjacentTraceRegion(1);
        ImGui::PopID();
    }

    if (!m_decodeResult.diagnostics.empty()) {
        ImGui::SameLine(0.0f, 12.0f);
        ImVec4 colour = m_decodeResult.HasErrors() ? ImVec4(1.0f, 0.35f, 0.3f, 1.0f)
                                                   : ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
        ImGui::TextColored(colour, "%d notice%s",
                           static_cast<int>(m_decodeResult.diagnostics.size()),
                           m_decodeResult.diagnostics.size() == 1 ? "" : "s");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (const auto& diagnostic : m_decodeResult.diagnostics)
                ImGui::TextWrapped("%s", diagnostic.message.c_str());
            ImGui::EndTooltip();
        }
    }
}

void BitplanesView::DrawControls()
{
    if (m_sourceMode == kSourceManual) {
        ImGui::SeparatorText("Manual geometry");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("Planes", &m_manualBplCount, 1, 8);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt("Width", &m_manualWidth, 16, 64);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt("Height", &m_manualHeight, 1, 16);
        m_manualWidth = std::clamp((m_manualWidth / 16) * 16, 16, 2048);
        m_manualHeight = std::clamp(m_manualHeight, 1, 576);
        if (ImGui::Button("Use detected geometry") && m_regsValid) {
            core::BitplaneDecodeOptions detectedOptions;
            const auto detected = core::BitplaneDecoder::Decode(m_regs, detectedOptions, {}, false);
            m_manualBplCount = std::max(1, detected.layout.bplCount);
            m_manualWidth = detected.layout.screenWidth;
            m_manualHeight = detected.layout.screenHeight;
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Plane addresses");
        ImGui::Checkbox("Use live register pointers", &m_useRegisterPointers);
        ImGui::BeginDisabled(m_useRegisterPointers);
        for (int plane = 0; plane < m_manualBplCount; ++plane) {
            char label[24];
            snprintf(label, sizeof(label), "BPL%d##manualptr", plane + 1);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputScalar(label, ImGuiDataType_U32, &m_manualPointers[plane], nullptr, nullptr,
                               "%06X", ImGuiInputTextFlags_CharsHexadecimal);
        }
        ImGui::EndDisabled();
        if (!m_useRegisterPointers && ImGui::Button("Copy live pointers") && m_regsValid) {
            for (int plane = 0; plane < kMaxPlanes; ++plane)
                m_manualPointers[plane] = core::SwapPtrWords(m_regs.bplpt[plane]);
        }
    }

    if (!m_traceMode &&
        ImGui::CollapsingHeader("Decode overrides", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_sourceMode != kSourceManual) {
            ImGui::Checkbox("Override width", &m_overrideWidth);
            if (m_overrideWidth) {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputInt("Width##override", &m_manualWidth, 16, 64);
            }
            ImGui::Checkbox("Override height", &m_overrideHeight);
            if (m_overrideHeight) {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputInt("Height##override", &m_manualHeight, 1, 16);
            }
            if (m_overrideWidth || m_overrideHeight) {
                if (ImGui::Button("Use detected geometry")) {
                    m_overrideWidth = false;
                    m_overrideHeight = false;
                }
            }
        }

        ImGui::Checkbox("Detect interleaving", &m_interleavedAuto);
        if (!m_interleavedAuto)
            ImGui::Checkbox("Interleaved", &m_interleavedForce);
        else
            ImGui::TextDisabled("Detected: %s", m_interleaved ? "interleaved" : "planar");

        ImGui::Checkbox("Override BPL1MOD", &m_useManualModulo1);
        if (m_useManualModulo1) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("BPL1MOD", &m_manualModulo1);
        }
        ImGui::Checkbox("Override BPL2MOD", &m_useManualModulo2);
        if (m_useManualModulo2) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("BPL2MOD", &m_manualModulo2);
        }

        ImGui::Checkbox("Explicit odd-plane stride", &m_useManualStride1);
        if (m_useManualStride1) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("Odd stride (bytes)", &m_manualStride1);
        }
        ImGui::Checkbox("Explicit even-plane stride", &m_useManualStride2);
        if (m_useManualStride2) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("Even stride (bytes)", &m_manualStride2);
        }
        m_manualStride1 = std::clamp(m_manualStride1, 1, 16384);
        m_manualStride2 = std::clamp(m_manualStride2, 1, 16384);
    }

    if (!m_traceMode && ImGui::CollapsingHeader("Palette"))
        DrawPalette();

    if (ImGui::CollapsingHeader("Registers")) {
        const BitplaneInstance* traceSource =
            m_sourceMode == kSourceAuto && m_autoUsingTrace &&
                    IsUsableTraceRegion(m_autoTraceRegion)
                ? &m_traceInstances[static_cast<size_t>(m_autoTraceRegion)]
                : nullptr;
        if (traceSource)
            ImGui::TextDisabled("Captured values for scanlines %d–%d",
                                static_cast<int>(traceSource->vpos_start),
                                static_cast<int>(traceSource->vpos_end));
        ImGui::TextDisabled("BPLCON0  $%04X", traceSource ? traceSource->bplcon0 : m_regs.bplcon0);
        ImGui::TextDisabled("BPLCON1  $%04X", m_regs.bplcon1);
        ImGui::TextDisabled("DDF       $%04X / $%04X",
                            traceSource ? traceSource->ddfstrt : m_regs.ddfstrt,
                            traceSource ? traceSource->ddfstop : m_regs.ddfstop);
        ImGui::TextDisabled("DIW       $%04X / $%04X", m_regs.diwstrt, m_regs.diwstop);
        ImGui::TextDisabled("DIWHIGH   $%04X", m_regs.diwhigh);
        ImGui::TextDisabled("FMODE     $%04X", m_regs.fmode);
        ImGui::TextDisabled("Stride    %d / %d bytes", m_decodeResult.layout.StrideBytes(0),
                            m_decodeResult.layout.StrideBytes(1));
        for (int plane = 0; plane < m_bplCount; ++plane) {
            ImGui::TextDisabled("BPL%dPT", plane + 1);
            ImGui::SameLine();
            char suffix[24];
            snprintf(suffix, sizeof(suffix), "inspector%d", plane);
            DrawPtrLink(suffix, m_bplBase[plane]);
        }
    }

    ImGuiTreeNodeFlags diagnosticFlags =
        m_decodeResult.HasErrors() ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
    if (!m_decodeResult.diagnostics.empty() &&
        ImGui::CollapsingHeader("Diagnostics", diagnosticFlags)) {
        for (const auto& diagnostic : m_decodeResult.diagnostics) {
            ImVec4 colour = diagnostic.severity == core::BitplaneDiagnosticSeverity::Error
                                ? ImVec4(1.0f, 0.35f, 0.3f, 1.0f)
                                : ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::BulletText("%s", diagnostic.message.c_str());
            ImGui::PopStyleColor();
            if (diagnostic.address != 0) {
                ImGui::Indent();
                char suffix[24];
                snprintf(suffix, sizeof(suffix), "diag%p", static_cast<const void*>(&diagnostic));
                DrawPtrLink(suffix, diagnostic.address);
                ImGui::Unindent();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// DrawGrid
// ---------------------------------------------------------------------------

void BitplanesView::DrawGrid()
{
    if (!m_regsValid) {
        ImGui::TextDisabled("No emulator register snapshot is available.");
        return;
    }
    if (m_bplCount == 0) {
        if (m_sourceMode == kSourceAuto)
            ImGui::TextDisabled(
                "No bitplanes are active at the frame boundary and no usable trace region was "
                "captured.");
        else
            ImGui::TextDisabled("No bitplanes are enabled in the frame-boundary BPLCON0.");
        return;
    }

    // Handle Ctrl+Scroll zoom over the preview area.
    if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            m_fitToWindow = false;
            m_zoom += wheel * 0.25f;
            m_zoom = std::clamp(m_zoom, 0.25f, 8.0f);
        }
    }

    // Aspect correction: HIRES pixels are half the width of LORES pixels (SHRES a quarter).
    float aspectX = 1.0f;
    float aspectY = 1.0f;
    if (!m_lockAspect)
        aspectX = 1.0f / static_cast<float>(m_decodeResult.layout.PixelsPerLores());

    // Blit-destination highlight — resolved once, drawn on the matching plane tile and the
    // composite (both render the front buffer, so the plane-local box applies to each).
    core::BlitOverlayBox blitBox;
    if (m_activeBlit)
        blitBox = core::MapBlitDestToPlane(*m_activeBlit, GetBitplaneLayout());

    // Show the complete layer stack in order, ending with the composite. Fit mode chooses the
    // grid shape that gives the largest possible previews while keeping every tile visible.
    const int totalTiles = m_bplCount + (m_showComposite ? 1 : 0);
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float spacingX = ImGui::GetStyle().ItemSpacing.x;
    const float spacingY = ImGui::GetStyle().ItemSpacing.y;
    const float headerHeight = ImGui::GetTextLineHeightWithSpacing();
    const float sourceWidth = std::max(1.0f, static_cast<float>(m_screenWidth) * aspectX);
    const float sourceHeight = std::max(1.0f, static_cast<float>(m_screenHeight) * aspectY);

    int tilesPerRow = 1;
    float scale = m_zoom;
    if (m_fitToWindow) {
        float bestScale = 0.05f;
        for (int columns = 1; columns <= totalTiles; ++columns) {
            const int rows = (totalTiles + columns - 1) / columns;
            const float cellWidth =
                (available.x - spacingX * static_cast<float>(columns - 1)) / columns;
            const float cellHeight =
                (available.y - spacingY * static_cast<float>(rows - 1)) / rows - headerHeight;
            const float candidate = std::min(cellWidth / sourceWidth, cellHeight / sourceHeight);
            if (candidate > bestScale) {
                bestScale = candidate;
                tilesPerRow = columns;
            }
        }
        scale = std::max(0.05f, bestScale);
    } else {
        const float tileWidth = sourceWidth * scale;
        tilesPerRow = std::clamp(
            static_cast<int>((available.x + spacingX) / (tileWidth + spacingX)), 1, totalTiles);
    }

    for (int tile = 0; tile < totalTiles; ++tile) {
        bool isComposite = (tile == m_bplCount);

        GLuint texId = isComposite ? m_compositeTex : m_planeTex[tile];
        int texW = isComposite ? m_compositeTexW : m_planeTexW[tile];
        int texH = isComposite ? m_compositeTexH : m_planeTexH[tile];
        uint32_t base = isComposite ? 0 : m_bplBase[tile];

        if (tile > 0 && tile % tilesPerRow != 0)
            ImGui::SameLine();

        ImGui::BeginGroup();

        // Tile header label — plane index plus a clickable pointer address
        if (isComposite) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Composite");
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "BPL%d", tile + 1);
            ImGui::SameLine(0.0f, 6.0f);
            char suffix[24];
            snprintf(suffix, sizeof(suffix), "tile%d", tile);
            DrawPtrLink(suffix, base);
        }

        if (texId && texW > 0 && texH > 0) {
            float dispW = texW * scale * aspectX;
            float dispH = texH * scale * aspectY;

            ImVec2 imgPos = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
            ImGui::Image((ImTextureID)(uintptr_t)texId, ImVec2(dispW, dispH));
            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);

            // Blit-destination highlight on this tile (matching plane or composite).
            if (blitBox.valid && (isComposite || tile == blitBox.plane)) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 tl(imgPos.x + blitBox.u0 * dispW, imgPos.y + blitBox.v0 * dispH);
                ImVec2 br(imgPos.x + blitBox.u1 * dispW, imgPos.y + blitBox.v1 * dispH);
                dl->AddRectFilled(tl, br, IM_COL32(90, 255, 120, 50));
                dl->AddRect(tl, br, IM_COL32(90, 255, 120, 235), 0.0f, 0, 2.0f);
            }

            // Hover tooltip
            if (ImGui::IsItemHovered()) {
                ImVec2 mousePos = ImGui::GetMousePos();
                float relX = (mousePos.x - imgPos.x) / (scale * aspectX);
                float relY = (mousePos.y - imgPos.y) / (scale * aspectY);

                int pixX = static_cast<int>(relX);
                int pixY = static_cast<int>(relY);

                if (pixX >= 0 && pixX < texW && pixY >= 0 && pixY < texH) {
                    // Magnified zoom region: 20x10 source pixels at 16x display scale
                    const int zoomSrcW = 20;
                    const int zoomSrcH = 10;
                    const float magScale = 16.0f;
                    const float tooltipW = zoomSrcW * magScale; // 320px fixed width

                    int srcX = std::clamp(pixX - zoomSrcW / 2, 0, std::max(0, texW - zoomSrcW));
                    int srcY = std::clamp(pixY - zoomSrcH / 2, 0, std::max(0, texH - zoomSrcH));

                    float u0 = srcX / (float)texW;
                    float v0 = srcY / (float)texH;
                    float u1 = (srcX + zoomSrcW) / (float)texW;
                    float v1 = (srcY + zoomSrcH) / (float)texH;

                    // Fix tooltip width so it doesn't jump as content changes
                    ImGui::SetNextWindowSizeConstraints(ImVec2(tooltipW, 0),
                                                        ImVec2(tooltipW, FLT_MAX));
                    ImGui::BeginTooltip();

                    // --- Magnified area at top ---
                    ImVec2 magStart = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddCallback(
                        ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
                    ImGui::Image((ImTextureID)(uintptr_t)texId,
                                 ImVec2(tooltipW, zoomSrcH * magScale), ImVec2(u0, v0),
                                 ImVec2(u1, v1));
                    ImGui::GetWindowDrawList()->AddCallback(
                        ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);

                    // Highlight rect around the pixel under the cursor
                    {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        int pRX = pixX - srcX;
                        int pRY = pixY - srcY;
                        ImVec2 hlMin =
                            ImVec2(magStart.x + pRX * magScale, magStart.y + pRY * magScale);
                        ImVec2 hlMax = ImVec2(hlMin.x + magScale, hlMin.y + magScale);
                        // Dark outer border for contrast
                        dl->AddRect(ImVec2(hlMin.x - 1, hlMin.y - 1),
                                    ImVec2(hlMax.x + 1, hlMax.y + 1), IM_COL32(0, 0, 0, 220), 0.0f,
                                    0, 1.5f);
                        // White highlight
                        dl->AddRect(hlMin, hlMax, IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
                    }

                    ImGui::Separator();

                    if (isComposite) {
                        // Color index from stored map
                        int ci = 0;
                        int idx = pixY * texW + pixX;
                        if (!m_compositeColorIdx.empty() && idx < (int)m_compositeColorIdx.size())
                            ci = m_compositeColorIdx[idx];

                        // Resolve colour for this index
                        uint8_t cr = 0, cg = 0, cb = 0;
                        int numColors = 1 << m_bplCount;
                        if (m_applyPalette && m_regsValid && ci < 32) {
                            uint16_t ocs = PreviewColor(ci);
                            cr = static_cast<uint8_t>(((ocs >> 8) & 0xF) * 17);
                            cg = static_cast<uint8_t>(((ocs >> 4) & 0xF) * 17);
                            cb = static_cast<uint8_t>((ocs & 0xF) * 17);
                        } else if (m_applyPalette && m_regsValid && ci >= 32 && ci < 64 &&
                                   m_decodeResult.layout.displayMode ==
                                       core::BitplaneDisplayMode::ExtraHalfBrite) {
                            uint16_t ocs = PreviewColor(ci - 32);
                            cr = static_cast<uint8_t>((((ocs >> 8) & 0xF) * 17) / 2);
                            cg = static_cast<uint8_t>((((ocs >> 4) & 0xF) * 17) / 2);
                            cb = static_cast<uint8_t>(((ocs & 0xF) * 17) / 2);
                        } else {
                            uint8_t v =
                                (ci == 0)
                                    ? 0
                                    : static_cast<uint8_t>((ci * 255) / std::max(1, numColors - 1));
                            cr = cg = cb = v;
                        }

                        ImGui::Text("Composite  pixel (%d, %d)", pixX, pixY);
                        // Color swatch + index on one line
                        ImGui::ColorButton(
                            "##pixcol", ImVec4(cr / 255.0f, cg / 255.0f, cb / 255.0f, 1.0f),
                            ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip,
                            ImVec2(16, 16));
                        ImGui::SameLine();
                        ImGui::Text("Index: %d  ($%02X)", ci, ci);
                        // Decimal RGB on its own line
                        ImGui::Text("RGB:   %d, %d, %d", cr, cg, cb);
                        // Hex on its own line
                        ImGui::Text("Hex:   #%02X%02X%02X", cr, cg, cb);
                    } else {
                        int bytesPerRow = texW / 8;
                        int modulo = (tile % 2 == 0) ? m_modulo1 : m_modulo2;
                        int rowStride = m_interleaved ? m_bplCount * bytesPerRow + modulo
                                                      : bytesPerRow + modulo;

                        uint32_t byteAddr = base + pixY * rowStride + pixX / 8;
                        int bitIdx = 7 - (pixX % 8);

                        ImGui::Text("Plane %d  pixel (%d, %d)", tile + 1, pixX, pixY);
                        ImGui::Text("Addr: $%06X  bit %d", byteAddr, bitIdx);
                    }

                    ImGui::EndTooltip();
                }
            }
        } else {
            // Placeholder when texture is not ready
            float dispW = std::max(64.0f, sourceWidth * scale);
            float dispH = std::max(32.0f, sourceHeight * scale);
            ImGui::Dummy(ImVec2(dispW, dispH));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetItemRectMin();
            ImVec2 p1 = ImGui::GetItemRectMax();
            dl->AddRect(p0, p1, IM_COL32(80, 80, 80, 200));
            ImVec2 mid = ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
            dl->AddText(ImVec2(mid.x - 20, mid.y - 7), IM_COL32(120, 120, 120, 255), "(empty)");
        }

        ImGui::EndGroup();
    }
}

// ---------------------------------------------------------------------------
// DrawPalette  (vertical swatch strip on the right)
// ---------------------------------------------------------------------------

void BitplanesView::DrawPalette()
{
    if (!m_regsValid || m_bplCount == 0) {
        ImGui::TextDisabled("-");
        return;
    }

    int numColors = std::min(1 << m_bplCount, 32);
    const float swatch = 25.0f;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::clamp(
        static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (swatch + gap)), 1, 8);
    for (int i = 0; i < numColors; i++) {
        const uint16_t ocs = PreviewColor(i) & 0x0FFF;
        const uint8_t r = static_cast<uint8_t>(((ocs >> 8) & 0xF) * 17);
        const uint8_t g = static_cast<uint8_t>(((ocs >> 4) & 0xF) * 17);
        const uint8_t b = static_cast<uint8_t>((ocs & 0xF) * 17);
        char id[24];
        snprintf(id, sizeof(id), "##palette%d", i);
        ImGui::ColorButton(id, ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f),
                           ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip,
                           ImVec2(swatch, swatch));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("COLOR%02d  $%03X  #%02X%02X%02X", i, ocs, r, g, b);
        if ((i + 1) % columns != 0 && i + 1 < numColors)
            ImGui::SameLine();
    }
}

// ---------------------------------------------------------------------------
// Draw  (main entry point called by DebuggerView)
// ---------------------------------------------------------------------------

void BitplanesView::Draw()
{
    const auto preferencesBefore = CurrentPreferences();
    DrawToolbar();
    EnsureDecoded();
    ImGui::Separator();
    DrawStatusBar();
    ImGui::Separator();

    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (m_traceMode) {
        if (!m_traceInstances.empty() &&
            (m_selectedTraceRow < 0 ||
             m_selectedTraceRow >= static_cast<int>(m_traceInstances.size())))
            m_selectedTraceRow = 0;

        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const bool sideBySide = !m_traceInstances.empty() && available.x >= 820.0f;
        if (sideBySide) {
            const float listWidth = std::clamp(available.x * 0.44f, 390.0f, 540.0f);
            ImGui::BeginChild("##bpltracelist", ImVec2(listWidth, 0.0f), true);
            DrawTraceTable();
            ImGui::EndChild();
            ImGui::SameLine(0.0f, spacing);
            ImGui::BeginChild("##bpltracedetail", ImVec2(0.0f, 0.0f), true);
            DrawTraceExpandedRow(m_selectedTraceRow);
            ImGui::EndChild();
        } else if (!m_traceInstances.empty()) {
            const float listHeight = std::clamp(available.y * 0.44f, 190.0f, 340.0f);
            ImGui::BeginChild("##bpltracelist", ImVec2(0.0f, listHeight), true);
            DrawTraceTable();
            ImGui::EndChild();
            ImGui::BeginChild("##bpltracedetail", ImVec2(0.0f, 0.0f), true);
            DrawTraceExpandedRow(m_selectedTraceRow);
            ImGui::EndChild();
        } else {
            DrawTraceTable();
        }
        if (m_userPreferences && CurrentPreferences() != preferencesBefore)
            m_userPreferences->SetBitplaneSettings(CurrentPreferences());
        return;
    }

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const bool sideBySide = m_showInspector && available.x >= 720.0f;
    if (sideBySide) {
        const float inspectorWidth = std::clamp(available.x * 0.30f, 270.0f, 360.0f);
        ImGui::BeginChild("##bplpreview", ImVec2(available.x - inspectorWidth - spacing, 0.0f),
                          false, ImGuiWindowFlags_HorizontalScrollbar);
        DrawGrid();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##bplinspector", ImVec2(inspectorWidth, 0.0f), true);
        DrawControls();
        ImGui::EndChild();
    } else if (m_showInspector) {
        const float previewHeight = std::max(140.0f, available.y * 0.62f);
        ImGui::BeginChild("##bplpreview", ImVec2(0.0f, previewHeight), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        DrawGrid();
        ImGui::EndChild();
        ImGui::BeginChild("##bplinspector", ImVec2(0.0f, 0.0f), true);
        DrawControls();
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("##bplpreview", ImVec2(0.0f, 0.0f), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        DrawGrid();
        ImGui::EndChild();
    }

    if (m_userPreferences && CurrentPreferences() != preferencesBefore)
        m_userPreferences->SetBitplaneSettings(CurrentPreferences());
}

} // namespace adm::ui
