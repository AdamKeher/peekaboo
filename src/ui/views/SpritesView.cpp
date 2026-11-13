#include "SpritesView.h"
#include "../TableLayout.h"

#include <glad/glad.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../core/AmigaScreenMap.h"
#include "../HyperlinkText.h"
#include "../OcsColor.h"
#include "EmulatorView.h"

namespace adm::ui {

// ----- static colour table ---------------------------------------------------

const ImU32 SpritesView::kSpriteColors[8] = {
    IM_COL32(0, 255, 255, 220),   // SPR0 cyan
    IM_COL32(255, 128, 0, 220),   // SPR1 orange
    IM_COL32(64, 255, 64, 220),   // SPR2 green
    IM_COL32(255, 64, 255, 220),  // SPR3 magenta
    IM_COL32(255, 255, 0, 220),   // SPR4 yellow
    IM_COL32(128, 128, 255, 220), // SPR5 lavender
    IM_COL32(255, 64, 64, 220),   // SPR6 red
    IM_COL32(64, 255, 200, 220),  // SPR7 teal
};

// ----- helpers ---------------------------------------------------------------

static GLuint UploadRGBATexture(GLuint existingTex, int w, int h, const uint8_t* rgba)
{
    GLuint tex = existingTex;
    if (!tex)
        glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// ----- ctor / dtor -----------------------------------------------------------

SpritesView::SpritesView(EmulatorView* emulatorView) : m_emulatorView(emulatorView)
{
    for (int i = 0; i < 8; i++)
        m_decoded[i].idx = i;
}

SpritesView::~SpritesView()
{
    for (int i = 0; i < 8; i++) {
        if (m_decoded[i].texId) {
            glDeleteTextures(1, &m_decoded[i].texId);
            m_decoded[i].texId = 0;
        }
        // Delete any per-instance textures built for trace mode
        for (auto& inst : m_decoded[i].instances) {
            if (inst.texId) {
                glDeleteTextures(1, &inst.texId);
                inst.texId = 0;
            }
        }
    }
    if (m_histTexId) {
        glDeleteTextures(1, &m_histTexId);
        m_histTexId = 0;
    }
}

void SpritesView::ReleaseInstanceTextures(int spriteIdx)
{
    for (auto& inst : m_decoded[spriteIdx].instances) {
        if (inst.texId) {
            glDeleteTextures(1, &inst.texId);
            inst.texId = 0;
        }
    }
}

void SpritesView::Clear()
{
    for (int i = 0; i < 8; i++) {
        auto& s = m_decoded[i];
        // Delete per-instance textures before clearing
        ReleaseInstanceTextures(i);
        s.instances.clear();
        s.active = false;
        s.ptr = 0;
        s.height = 0;
        s.texH = 0;
        s.texW = 0;
        s.texDirty = true;
    }
    m_traceRows.clear();
    m_histDirty = true;
    m_cachedRegsValid = false;
    m_lastDecodeEmuFrame = UINT64_MAX; // force a re-decode on next frame
}

// ----- decode ----------------------------------------------------------------

void SpritesView::EnsureDecoded()
{
    // Gated on the emulator frame only (unlike BitplanesView, which also hashes its
    // decode-affecting manual controls): sprite decoding has no user-tunable inputs, so the frame
    // counter alone fully determines whether a re-decode is needed.
    uint64_t emuFrame = m_emulatorView ? m_emulatorView->GetEmulatorFrameCount() : 0;
    if (emuFrame == m_lastDecodeEmuFrame)
        return;
    m_lastDecodeEmuFrame = emuFrame;
    DecodeSprites();
}

void SpritesView::DecodeSprites()
{
    m_cachedRegsValid = false;

    if (!m_emulatorView || !m_emulatorView->IsRunning()) {
        for (int i = 0; i < 8; i++) {
            auto& s = m_decoded[i];
            s.instances.clear();
            s.active = false;
            s.ptr = 0;
            s.height = 0;
            s.texH = 0;
            s.texW = 0;
            s.hstart = 0;
            s.vstart = 0;
            s.vstop = 0;
            s.attached = false;
            s.isEvenOfPair = false;
        }
        return;
    }

    core::Custom regs{};
    if (!m_emulatorView->GetEmulator()->GetCustomRegisters(regs))
        return;

    m_cachedRegs = regs;
    m_cachedRegsValid = true;

    auto regions = m_emulatorView->GetFreshMemoryRegions();

    // DMACONR (0x002), not DMACON (0x096): the latter is a raw write shadow holding the last
    // SET/CLR word the CPU wrote, so a clear is indistinguishable from a set. DMACONR is live.
    bool sprenEnabled = (regs.dmaconr & (1 << 5)) != 0;

    // First pass: decode registers for each sprite
    for (int i = 0; i < 8; i++) {
        auto& s = m_decoded[i];
        s.idx = i;
        // sprpt values from GetCustomRegisters() are word-swapped (the memcpy lands SPR0PTL in the
        // high 16 bits and SPR0PTH in the low 16 bits); swap halves to get the real Amiga address.
        s.ptr = core::SwapPtrWords(regs.sprpt[i]);
        s.pos_reg = regs.spr[i].pos;
        s.ctl_reg = regs.spr[i].ctl;

        // Prefer reading pos/ctl directly from memory at the sprite pointer.
        // Custom registers may be stale when sprite DMA is disabled (SPREN=0),
        // since DMA never ran to load them from the sprite data in chip RAM.
        if (s.ptr != 0) {
            uint16_t mem_pos, mem_ctl;
            if (ReadChipWord(s.ptr + 0, mem_pos, regions) &&
                ReadChipWord(s.ptr + 2, mem_ctl, regions)) {
                s.pos_reg = mem_pos;
                s.ctl_reg = mem_ctl;
            }
        }

        uint16_t pos = s.pos_reg;
        uint16_t ctl = s.ctl_reg;

        s.vstart = ((pos >> 8) & 0xFF) | ((ctl & 4) ? 0x100 : 0);
        s.vstop = ((ctl >> 8) & 0xFF) | ((ctl & 2) ? 0x100 : 0);
        s.hstart = ((pos & 0xFF) << 1) | (ctl & 1);
        s.height = (s.vstop > s.vstart) ? (s.vstop - s.vstart) : 0;

        s.attached = (i & 1) && ((ctl & 0x80) != 0);
        s.isEvenOfPair = false; // set in second pass

        s.active = s.height > 0 && sprenEnabled;
    }

    // Second pass: mark isEvenOfPair on even sprites whose odd partner has ATT set
    for (int i = 1; i < 8; i += 2) {
        if (m_decoded[i].attached)
            m_decoded[i - 1].isEvenOfPair = true;
    }

    // Third pass: set colour bases and palettes
    for (int i = 0; i < 8; i++) {
        auto& s = m_decoded[i];
        if (s.attached || s.isEvenOfPair) {
            s.colorBase = 16;
            s.numColors = 15;
            s.palette[0] = IM_COL32(0, 0, 0, 0);
            for (int c = 1; c <= 15; c++) {
                int ci = 16 + c;
                s.palette[c] =
                    (ci < 32) ? OcsColorToImU32(regs.color[ci]) : IM_COL32(128, 128, 128, 255);
            }
        } else {
            s.colorBase = 16 + 4 * (i / 2);
            s.numColors = 3;
            s.palette[0] = IM_COL32(0, 0, 0, 0);
            for (int c = 1; c <= 3; c++) {
                int ci = s.colorBase + c;
                s.palette[c] =
                    (ci < 32) ? OcsColorToImU32(regs.color[ci]) : IM_COL32(128, 128, 128, 255);
            }
        }
        s.texDirty = true; // always rebuild preview textures when decoding
    }

    // Build instances from SprptEvents or fall back to register-state single instance
    adm::AmigaEmulator::SprptEvent events[adm::AmigaEmulator::kMaxSprptEvents];
    int eventCount = m_emulatorView->GetSprptEvents(events, adm::AmigaEmulator::kMaxSprptEvents);
    m_lastSprptEventCount = eventCount;

    if (eventCount > 0) {
        BuildInstances(events, eventCount, regions);
    } else {
        // Fallback: synthesise instances per sprite from current register state
        for (int i = 0; i < 8; i++) {
            ReleaseInstanceTextures(i); // per-frame path — clearing without this leaked textures
            m_decoded[i].instances.clear();
            auto& s = m_decoded[i];
            if (s.ptr != 0) {
                uint32_t current_ptr = s.ptr;
                int safety_counter = 0;
                int prevVstop = -1; // forward-monotonic guard (see BuildInstances)
                while (current_ptr != 0 && safety_counter < 64) {
                    uint16_t pos = 0, ctl = 0;
                    if (!ReadChipWord(current_ptr + 0, pos, regions))
                        break;
                    if (!ReadChipWord(current_ptr + 2, ctl, regions))
                        break;

                    if (pos == 0 && ctl == 0)
                        break; // End of sprite data

                    SpriteInstance inst;
                    inst.spriteIdx = i;
                    inst.vpos_armed = 0;
                    inst.ptr = current_ptr;
                    inst.vstart = ((pos >> 8) & 0xFF) | ((ctl & 4) ? 0x100 : 0);
                    inst.vstop = ((ctl >> 8) & 0xFF) | ((ctl & 2) ? 0x100 : 0);
                    inst.hstart = ((pos & 0xFF) << 1) | (ctl & 1);

                    // Auto-continuation can only move down the screen; a control word above the
                    // previous image's VSTOP is garbage / an implicit terminator, not a real image.
                    if (prevVstop >= 0 && inst.vstart < prevVstop)
                        break;

                    s.instances.push_back(inst);

                    prevVstop = inst.vstop;
                    int height = inst.vstop - inst.vstart;
                    if (height <= 0)
                        height = 0;

                    current_ptr += 4 + height * 4;
                    safety_counter++;
                }
            }
        }
    }

    // Derive per-instance palettes from copper events.
    // This correctly handles the common pattern where SPRxPT is armed first and the Copper
    // changes COLOR16-31 afterwards (at the sprite's vstart scanline).
    // m_copperEventBuf is a class member (heap) to avoid large stack allocations.
    {
        const int maxCopperEvts = adm::AmigaEmulator::kMaxCopperExecEvents;
        m_copperEventBuf.resize(static_cast<size_t>(maxCopperEvts));
        int copperCount = m_emulatorView->GetEmulator()->GetCopperExecEvents(
            m_copperEventBuf.data(), maxCopperEvts);
        AssignInstanceColorsFromCopper(regs, m_copperEventBuf.data(), copperCount);
    }

    // Build preview textures (per-channel, for default mode)
    for (int i = 0; i < 8; i++)
        BuildSpriteTexture(i, regions, regs);

    // Rebuild flat trace-row list in chronological order (by vpos_armed)
    m_traceRows.clear();
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < static_cast<int>(m_decoded[i].instances.size()); k++)
            m_traceRows.push_back({i, k});
    // Sort by vstart (when the sprite actually appears on screen), with channel as tiebreaker
    std::sort(m_traceRows.begin(), m_traceRows.end(), [this](const TraceRow& a, const TraceRow& b) {
        const auto& ia = m_decoded[a.spriteIdx].instances[static_cast<size_t>(a.instIdx)];
        const auto& ib = m_decoded[b.spriteIdx].instances[static_cast<size_t>(b.instIdx)];
        if (ia.vstart != ib.vstart)
            return ia.vstart < ib.vstart;
        return a.spriteIdx < b.spriteIdx;
    });

    // Build per-instance textures in both modes: the trace table cells and the unified detail
    // panel's enlarged preview both consume inst.texId. Textures are 16×height and decode is
    // emulator-frame-gated, so the cost is negligible.
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < static_cast<int>(m_decoded[i].instances.size()); k++)
            BuildInstanceTexture(i, k, regions);

    m_histDirty = true;
}

void SpritesView::BuildInstances(const adm::AmigaEmulator::SprptEvent* events, int count,
                                 const std::vector<adm::AmigaEmulator::MemoryRegion>& regions)
{
    for (int i = 0; i < 8; i++) {
        // This runs every emulated frame and BuildInstanceTexture glGenTextures a fresh id per
        // instance — clearing without deleting leaked one GL texture per sprite per frame.
        ReleaseInstanceTextures(i);
        m_decoded[i].instances.clear();
    }

    // Group SPRxPTL-write events per channel, preserving chronological order. Each event is one
    // explicit re-point of the sprite pointer (the emulator only records actual SPRxPTL writes —
    // hardware DMA auto-advance is NOT recorded). After a re-point the sprite DMA auto-continues
    // fetching contiguous control words until the *next* re-point of that channel overrides the
    // pointer, so a given event's auto-continuation is only valid for scanlines before the next
    // event's arm line.
    int chanEvents[8][adm::AmigaEmulator::kMaxSprptEvents];
    int chanEventCount[8] = {0};
    for (int e = 0; e < count; e++) {
        int idx = events[e].spriteIdx;
        if (idx < 0 || idx > 7)
            continue;
        chanEvents[idx][chanEventCount[idx]++] = e;
    }

    // Each channel's sprite-data region begins at its base pointer (the lowest address this
    // channel is pointed at this frame). One channel's auto-continuation walk must never read
    // into *another* channel's region: when the contiguous chain reaches the next channel's data
    // it's no longer this sprite's images but the neighbour's (often misaligned, producing
    // impossible vstart/vstop). So the nearest higher foreign base is a hard upper bound on the
    // walk. Use each channel's min event ptr, falling back to the decoded SPRxPT register value.
    uint32_t chanBase[8];
    for (int idx = 0; idx < 8; idx++) {
        uint32_t base = m_decoded[idx].ptr;
        for (int j = 0; j < chanEventCount[idx]; j++) {
            uint32_t p = events[chanEvents[idx][j]].ptr;
            if (p != 0 && (base == 0 || p < base))
                base = p;
        }
        chanBase[idx] = base;
    }

    for (int idx = 0; idx < 8; idx++) {
        for (int j = 0; j < chanEventCount[idx]; j++) {
            const auto& ev = events[chanEvents[idx][j]];

            // The next re-point of this channel (if any) bounds how far the contiguous
            // auto-continuation walk may run: any image whose VSTART falls at/after the next
            // arm line belongs to that re-point, not this one. Without this bound a re-pointed
            // (multiplexed) sprite spills phantom images into the chip RAM that happens to follow
            // its single real image.
            int boundVpos = (j + 1 < chanEventCount[idx])
                                ? static_cast<int>(events[chanEvents[idx][j + 1]].vpos)
                                : INT_MAX;

            // Nearest higher foreign channel base: a hard memory ceiling for this walk. Reading a
            // control-word pair that would cross into it means we've left this sprite's region.
            uint32_t nextBase = 0;
            for (int o = 0; o < 8; o++) {
                if (o == idx || chanBase[o] == 0)
                    continue;
                if (chanBase[o] > ev.ptr && (nextBase == 0 || chanBase[o] < nextBase))
                    nextBase = chanBase[o];
            }

            uint32_t current_ptr = ev.ptr;
            int safety_counter = 0;
            int prevVstop = -1; // previous image's VSTOP in this walk (forward-monotonic guard)

            while (current_ptr != 0 && safety_counter < 64) {
                // Stop if this image's control words would spill into the next channel's region.
                if (nextBase != 0 && current_ptr + 4 > nextBase)
                    break;

                uint16_t pos = 0, ctl = 0;
                if (!ReadChipWord(current_ptr + 0, pos, regions))
                    break;
                if (!ReadChipWord(current_ptr + 2, ctl, regions))
                    break;

                if (pos == 0 && ctl == 0)
                    break; // End-of-list terminator

                int vstart = ((pos >> 8) & 0xFF) | ((ctl & 4) ? 0x100 : 0);
                int vstop = ((ctl >> 8) & 0xFF) | ((ctl & 2) ? 0x100 : 0);
                int hstart = ((pos & 0xFF) << 1) | (ctl & 1);

                // This image's display range belongs to the next re-point — stop the walk here.
                if (vstart >= boundVpos)
                    break;

                // Sprite DMA auto-continuation can only move *down* the screen: a fetched
                // control word whose VSTART is above the previous image's VSTOP can never be
                // displayed (the beam is already past it) and is therefore garbage / an implicit
                // terminator, not a real continued image. This is what kills the phantom
                // instances a re-pointed sprite would otherwise pick up from trailing chip RAM.
                if (prevVstop >= 0 && vstart < prevVstop)
                    break;

                SpriteInstance inst;
                inst.spriteIdx = idx;
                inst.vpos_armed = ev.vpos;
                inst.ptr = current_ptr;
                inst.vstart = vstart;
                inst.vstop = vstop;
                inst.hstart = hstart;

                // Build per-instance palette from the COLOR16-31 snapshot in the event.
                // colorBase for this sprite pair: 16 + (idx/2)*4
                // ev.colors[i] = COLOR(16+i), so ev.colors[0] = COLOR16, ev.colors[1] = COLOR17.
                {
                    bool hasColors = false;
                    for (int ci = 0; ci < 16; ci++) {
                        if (ev.colors[ci] != 0) {
                            hasColors = true;
                            break;
                        }
                    }
                    if (hasColors) {
                        int colorBase = 16 + (idx / 2) * 4; // first COLOR for this sprite pair
                        bool isAttachedPair =
                            m_decoded[idx].attached || m_decoded[idx].isEvenOfPair;
                        inst.paletteValid = true;
                        inst.palette[0] = IM_COL32(0, 0, 0, 0); // transparent
                        if (isAttachedPair) {
                            // 15-colour attached pair — use all of ev.colors[1..15]
                            for (int c = 1; c <= 15; c++)
                                inst.palette[c] = OcsColorToImU32(ev.colors[c]);
                        } else {
                            // Normal 3-colour sprite — colours at colorBase offset in the event.
                            int offset = colorBase - 16; // index into ev.colors (starts at COLOR16)
                            for (int c = 1; c <= 3; c++) {
                                int ci = offset + c;
                                inst.palette[c] = (ci < 16) ? OcsColorToImU32(ev.colors[ci])
                                                            : IM_COL32(128, 128, 128, 255);
                            }
                        }
                    }
                }

                // Avoid duplicate instances if a later re-point points at the same image again.
                bool exists = false;
                for (const auto& existing : m_decoded[idx].instances) {
                    if (existing.ptr == current_ptr && existing.vstart == inst.vstart) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    m_decoded[idx].instances.push_back(inst);
                }

                prevVstop = vstop;
                int height = vstop - vstart;
                if (height <= 0)
                    height = 0;

                current_ptr += 4 + height * 4;
                safety_counter++;
            }
        }
    }
}

bool SpritesView::ReadChipWord(uint32_t addr, uint16_t& out,
                               const std::vector<adm::AmigaEmulator::MemoryRegion>& regions) const
{
    for (const auto& r : regions) {
        if (!r.hostPtr)
            continue;
        if (addr >= r.amigaAddr && addr + 2 <= r.amigaAddr + r.size) {
            const uint8_t* p = r.hostPtr + (addr - r.amigaAddr);
            out = static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
            return true;
        }
    }
    return false;
}

// ----- per-instance color assignment from copper events ---------------------

void SpritesView::AssignInstanceColorsFromCopper(
    const core::Custom& regs, const adm::AmigaEmulator::CopperExecEvent* copperEvents,
    int copperCount)
{
    // Build a flat list of all instances sorted by vstart so we can do a single
    // forward pass through the copper event log.
    struct InstRef
    {
        int vstart;
        int si;
        int ki;
    };
    std::vector<InstRef> allInsts;
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < static_cast<int>(m_decoded[i].instances.size()); k++)
            allInsts.push_back({m_decoded[i].instances[static_cast<size_t>(k)].vstart, i, k});

    if (allInsts.empty())
        return;

    std::sort(allInsts.begin(), allInsts.end(),
              [](const InstRef& a, const InstRef& b) { return a.vstart < b.vstart; });

    // Initialize live color state from the end-of-frame hardware snapshot.
    // COLOR16-31 are at regs.color[16..31].
    uint16_t liveColor[16];
    for (int c = 0; c < 16; c++)
        liveColor[c] = static_cast<uint16_t>(regs.color[16 + c] & 0x0fff);

    // Forward pass: replay copper MOVE events to COLOR16-31 and snapshot at each inst's vstart.
    // Copper MOVE: (word1 & 1) == 0; register offset = word1 & 0x01FE.
    // COLOR00..31 live at offsets 0x180..0x1BE (2 bytes apart).
    // COLOR16 = offset 0x1A0, COLOR31 = offset 0x1BE.
    constexpr uint16_t kColor16Reg = 0x1A0u;
    constexpr uint16_t kColor31Reg = 0x1BEu;

    int instPos = 0;
    int cevIdx = 0;

    // We need to walk copper events and instances together by scanline.
    // For each instance at vstart V, the active palette is whatever liveColor is
    // AFTER all copper moves that happened at scanlines < V (and before moves at V).
    // We hand the instance the colors that the Copper has set up before the sprite starts.
    //
    // Edge case: copper events come from the current frame's CCK window; for instances
    // near vpos=0 there may be no preceding copper moves in the event log, so we keep
    // the end-of-frame fallback colours set above.

    while (instPos < static_cast<int>(allInsts.size())) {
        int targetVpos = allInsts[static_cast<size_t>(instPos)].vstart;

        // Consume all copper events with vpos < targetVpos
        while (cevIdx < copperCount) {
            const auto& cev = copperEvents[cevIdx];
            if (cev.vpos >= static_cast<uint16_t>(targetVpos))
                break;
            // Is it a MOVE to a sprite color register?
            if ((cev.word1 & 1u) == 0) {
                uint16_t reg = cev.word1 & 0x01FEu;
                if (reg >= kColor16Reg && reg <= kColor31Reg) {
                    int ci = (reg - kColor16Reg) / 2; // 0..15
                    liveColor[ci] = cev.word2 & 0x0fffu;
                }
            }
            ++cevIdx;
        }

        // Assign the current liveColor to all instances at this vstart
        while (instPos < static_cast<int>(allInsts.size()) &&
               allInsts[static_cast<size_t>(instPos)].vstart == targetVpos) {
            auto& inst =
                m_decoded[allInsts[static_cast<size_t>(instPos)].si]
                    .instances[static_cast<size_t>(allInsts[static_cast<size_t>(instPos)].ki)];

            // Only override when there's at least one non-zero sprite colour
            bool hasColor = false;
            for (int c = 1; c <= 15 && !hasColor; c++)
                if (liveColor[c] != 0)
                    hasColor = true;

            if (hasColor) {
                inst.paletteValid = true;
                inst.palette[0] = IM_COL32(0, 0, 0, 0); // transparent

                int si = allInsts[static_cast<size_t>(instPos)].si;
                bool isPair = m_decoded[si].attached || m_decoded[si].isEvenOfPair;
                if (isPair) {
                    for (int c = 1; c <= 15; c++)
                        inst.palette[c] = OcsColorToImU32(liveColor[c]);
                } else {
                    // Normal 2bpp: colours at colorBase offset within COLOR16-31
                    int colorBase = 16 + 4 * (si / 2); // 16, 20, 24, 28 for pairs 0-3
                    int offset = colorBase - 16;       // 0, 4, 8, 12
                    for (int c = 1; c <= 3; c++) {
                        int ci = offset + c;
                        inst.palette[c] = (ci < 16) ? OcsColorToImU32(liveColor[ci])
                                                    : IM_COL32(128, 128, 128, 255);
                    }
                }
            }
            ++instPos;
        }
    }
}

// ----- sprite preview textures -----------------------------------------------

void SpritesView::BuildSpriteTexture(int idx,
                                     const std::vector<adm::AmigaEmulator::MemoryRegion>& regions,
                                     const core::Custom& /*regs*/)
{
    auto& s = m_decoded[idx];
    if (!s.texDirty)
        return;
    s.texDirty = false;

    // Odd sprite of an attached pair: no individual texture
    if (s.attached) {
        if (s.texId) {
            glDeleteTextures(1, &s.texId);
            s.texId = 0;
        }
        s.texW = s.texH = 0;
        return;
    }

    const int maxHeight = 512;
    int height = s.height;
    if (height <= 0 || height > maxHeight || s.ptr == 0) {
        if (s.texId) {
            glDeleteTextures(1, &s.texId);
            s.texId = 0;
        }
        s.texW = s.texH = 0;
        return;
    }

    constexpr int W = 16;
    std::vector<uint8_t> pixels(static_cast<size_t>(W * height * 4), 0);

    if (s.isEvenOfPair) {
        // Attached pair: combine even and odd sprite bitplanes → 4bpp, 15 colours
        int oddIdx = idx + 1;
        auto& odd = m_decoded[oddIdx];
        uint32_t evenPtr = s.ptr;
        uint32_t oddPtr = odd.ptr;

        for (int l = 0; l < height; l++) {
            uint16_t eA = 0, eB = 0, oA = 0, oB = 0;
            ReadChipWord(evenPtr + 4 + l * 4 + 0, eA, regions);
            ReadChipWord(evenPtr + 4 + l * 4 + 2, eB, regions);
            ReadChipWord(oddPtr + 4 + l * 4 + 0, oA, regions);
            ReadChipWord(oddPtr + 4 + l * 4 + 2, oB, regions);

            for (int b = 0; b < W; b++) {
                int bit = 15 - b;
                int colorIdx = (((oB >> bit) & 1) << 3) | (((oA >> bit) & 1) << 2) |
                               (((eB >> bit) & 1) << 1) | ((eA >> bit) & 1);

                uint8_t* p = &pixels[static_cast<size_t>((l * W + b) * 4)];
                if (colorIdx == 0) {
                    uint8_t v = static_cast<uint8_t>(((b + l) & 1) ? 80 : 40);
                    p[0] = p[1] = p[2] = v;
                    p[3] = 255;
                } else {
                    ImU32 col = s.palette[colorIdx];
                    p[0] = (col >> 0) & 0xFF;
                    p[1] = (col >> 8) & 0xFF;
                    p[2] = (col >> 16) & 0xFF;
                    p[3] = 255;
                }
            }
        }
    } else {
        // Normal 2bpp sprite, 3 usable colours
        uint32_t ptr = s.ptr;
        for (int l = 0; l < height; l++) {
            uint16_t dataa = 0, datab = 0;
            ReadChipWord(ptr + 4 + l * 4 + 0, dataa, regions);
            ReadChipWord(ptr + 4 + l * 4 + 2, datab, regions);

            for (int b = 0; b < W; b++) {
                int bit = 15 - b;
                int colorIdx = (((datab >> bit) & 1) << 1) | ((dataa >> bit) & 1);

                uint8_t* p = &pixels[static_cast<size_t>((l * W + b) * 4)];
                if (colorIdx == 0) {
                    uint8_t v = static_cast<uint8_t>(((b + l) & 1) ? 80 : 40);
                    p[0] = p[1] = p[2] = v;
                    p[3] = 255;
                } else if (colorIdx <= 3) {
                    ImU32 col = s.palette[colorIdx];
                    p[0] = (col >> 0) & 0xFF;
                    p[1] = (col >> 8) & 0xFF;
                    p[2] = (col >> 16) & 0xFF;
                    p[3] = 255;
                }
            }
        }
    }

    s.texId = UploadRGBATexture(s.texId, W, height, pixels.data());
    s.texW = W;
    s.texH = height;
}

// ----- per-instance texture (trace mode) ------------------------------------

void SpritesView::BuildInstanceTexture(int sprIdx, int instIdx,
                                       const std::vector<adm::AmigaEmulator::MemoryRegion>& regions)
{
    auto& inst = m_decoded[sprIdx].instances[instIdx];
    const auto& s = m_decoded[sprIdx];

    // Odd attached sprite — skip; no individual texture
    if (s.attached) {
        if (inst.texId) {
            glDeleteTextures(1, &inst.texId);
            inst.texId = 0;
        }
        inst.texW = inst.texH = 0;
        return;
    }

    int height = inst.vstop - inst.vstart;
    if (height <= 0 || height > 512 || inst.ptr == 0) {
        if (inst.texId) {
            glDeleteTextures(1, &inst.texId);
            inst.texId = 0;
        }
        inst.texW = inst.texH = 0;
        return;
    }

    // Use per-instance palette if available (captured from color regs at arm time),
    // otherwise fall back to the channel's end-of-frame palette.
    const ImU32* pal = inst.paletteValid ? inst.palette : s.palette;

    constexpr int W = 16;
    std::vector<uint8_t> pixels(static_cast<size_t>(W * height * 4), 0);

    if (s.isEvenOfPair) {
        // Attached pair — combine this instance's even data with the odd sprite's matching
        // instance (if available), otherwise fall back to the odd sprite's first instance ptr.
        int oddIdx = sprIdx + 1;
        uint32_t oddPtr = 0;
        if (oddIdx < 8) {
            // Look for an odd-sprite instance with matching vstart
            for (const auto& oi : m_decoded[oddIdx].instances) {
                if (oi.vstart == inst.vstart) {
                    oddPtr = oi.ptr;
                    break;
                }
            }
            // Fallback: use the first instance of the odd sprite
            if (oddPtr == 0 && !m_decoded[oddIdx].instances.empty())
                oddPtr = m_decoded[oddIdx].instances[0].ptr;
            if (oddPtr == 0)
                oddPtr = m_decoded[oddIdx].ptr;
        }

        for (int l = 0; l < height; l++) {
            uint16_t eA = 0, eB = 0, oA = 0, oB = 0;
            ReadChipWord(inst.ptr + 4 + l * 4 + 0, eA, regions);
            ReadChipWord(inst.ptr + 4 + l * 4 + 2, eB, regions);
            if (oddPtr) {
                ReadChipWord(oddPtr + 4 + l * 4 + 0, oA, regions);
                ReadChipWord(oddPtr + 4 + l * 4 + 2, oB, regions);
            }
            for (int b = 0; b < W; b++) {
                int bit = 15 - b;
                int colorIdx = (((oB >> bit) & 1) << 3) | (((oA >> bit) & 1) << 2) |
                               (((eB >> bit) & 1) << 1) | ((eA >> bit) & 1);
                uint8_t* p = &pixels[static_cast<size_t>((l * W + b) * 4)];
                if (colorIdx == 0) {
                    uint8_t v = static_cast<uint8_t>(((b + l) & 1) ? 80 : 40);
                    p[0] = p[1] = p[2] = v;
                    p[3] = 255;
                } else {
                    ImU32 col = pal[colorIdx];
                    p[0] = (col >> 0) & 0xFF;
                    p[1] = (col >> 8) & 0xFF;
                    p[2] = (col >> 16) & 0xFF;
                    p[3] = 255;
                }
            }
        }
    } else {
        // Normal 2bpp sprite
        for (int l = 0; l < height; l++) {
            uint16_t dataa = 0, datab = 0;
            ReadChipWord(inst.ptr + 4 + l * 4 + 0, dataa, regions);
            ReadChipWord(inst.ptr + 4 + l * 4 + 2, datab, regions);
            for (int b = 0; b < W; b++) {
                int bit = 15 - b;
                int colorIdx = (((datab >> bit) & 1) << 1) | ((dataa >> bit) & 1);
                uint8_t* p = &pixels[static_cast<size_t>((l * W + b) * 4)];
                if (colorIdx == 0) {
                    uint8_t v = static_cast<uint8_t>(((b + l) & 1) ? 80 : 40);
                    p[0] = p[1] = p[2] = v;
                    p[3] = 255;
                } else if (colorIdx <= 3) {
                    ImU32 col = pal[colorIdx];
                    p[0] = (col >> 0) & 0xFF;
                    p[1] = (col >> 8) & 0xFF;
                    p[2] = (col >> 16) & 0xFF;
                    p[3] = 255;
                }
            }
        }
    }

    inst.texId = UploadRGBATexture(inst.texId, W, height, pixels.data());
    inst.texW = W;
    inst.texH = height;
}

// ----- histogram -------------------------------------------------------------

void SpritesView::RebuildHistogram()
{
    if (!m_histDirty)
        return;
    m_histDirty = false;

    constexpr int W = kHistW;
    constexpr int H = kHistH;
    std::vector<uint8_t> pixels(static_cast<size_t>(W * H * 4), 0);

    // Background: dark grey
    for (int i = 0; i < W * H; i++) {
        pixels[i * 4 + 0] = 30;
        pixels[i * 4 + 1] = 30;
        pixels[i * 4 + 2] = 30;
        pixels[i * 4 + 3] = 255;
    }

    // Each sprite occupies 10px wide column
    for (int i = 0; i < 8; i++) {
        const auto& s = m_decoded[i];
        ImU32 col = kSpriteColors[i];
        uint8_t cr = (col >> 0) & 0xFF;
        uint8_t cg = (col >> 8) & 0xFF;
        uint8_t cb = (col >> 16) & 0xFF;

        int colX = i * 10;

        for (const auto& inst : s.instances) {
            int scanStart = std::max(0, std::min(H - 1, inst.vstart));
            int scanEnd = std::max(0, std::min(H, inst.vstop));
            for (int y = scanStart; y < scanEnd; y++) {
                for (int x = colX; x < colX + 9; x++) {
                    uint8_t* p = &pixels[static_cast<size_t>((y * W + x) * 4)];
                    p[0] = cr;
                    p[1] = cg;
                    p[2] = cb;
                    p[3] = 200;
                }
            }
        }
    }

    m_histTexId = UploadRGBATexture(m_histTexId, W, H, pixels.data());
}

// ----- draw ------------------------------------------------------------------

void SpritesView::SetAllBBoxes(bool show) { m_bboxMask = show ? 0xFF : 0x00; }

void SpritesView::DrawAddressLink(uint32_t addr)
{
    if (addr == 0) {
        ImGui::TextDisabled("---");
        return;
    }
    if (!m_onJumpToMemory) {
        ImGui::Text("$%06X", addr);
        return;
    }
    ImGui::TextColored(kHyperlinkColor, "$%06X", addr);
    bool clicked = false;
    HoverableLink(&clicked);
    if (clicked)
        m_onJumpToMemory(addr);
}

void SpritesView::DrawSpriteEnableCheckbox(int spriteIdx)
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu) {
        bool dummy = true;
        ImGui::BeginDisabled();
        ImGui::Checkbox("##en", &dummy);
        ImGui::EndDisabled();
        return;
    }

    uint8_t mask = emu->GetSpriteMask();
    bool on = (mask & (1 << spriteIdx)) != 0;
    if (ImGui::Checkbox("##en", &on)) {
        if (on)
            mask |= static_cast<uint8_t>(1 << spriteIdx);
        else
            mask &= static_cast<uint8_t>(~(1 << spriteIdx));
        emu->SetSpriteMask(mask); // re-renders the frame itself
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Enable SPR%d rendering (synced with the Display window)", spriteIdx);
}

// Resizable splitter bar drawn between two adjacent panels. Returns the signed drag delta
// (mouse movement along the split axis) while the handle is held, else 0.
static float SpritesSplitter(const char* id, bool vertical, float longAxis, float thickness)
{
    ImGui::InvisibleButton(id,
                           vertical ? ImVec2(thickness, longAxis) : ImVec2(longAxis, thickness));
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();
    if (hovered || active)
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    ImU32 col = active    ? IM_COL32(130, 130, 130, 255)
                : hovered ? IM_COL32(100, 100, 100, 255)
                          : IM_COL32(60, 60, 60, 255);
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                              col);
    if (active)
        return vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
    return 0.0f;
}

void SpritesView::Draw()
{
    EnsureDecoded();

    bool running = m_emulatorView && m_emulatorView->IsRunning();

    // --- Header ---
    ImGui::BeginDisabled(!running);
    if (running && m_cachedRegsValid) {
        bool sprenEnabled = (m_cachedRegs.dmaconr & (1 << 5)) != 0;
        ImVec4 col = sprenEnabled ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(col, "SPREN: %s", sprenEnabled ? "ON" : "OFF");
        ImGui::SameLine(0, 16);

        int activeCount = 0;
        for (int i = 0; i < 8; i++)
            if (m_decoded[i].active)
                activeCount++;
        ImGui::Text("Active: %d / 8", activeCount);

        ImGui::SameLine(0, 16);
        if (m_lastSprptEventCount > 0) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Mux: %d evt",
                               m_lastSprptEventCount);
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Mux: no events (rebuild DLL?)");
        }

        ImGui::SameLine(0, 16);
        if (ImGui::Checkbox("Trace##spr", &m_traceMode))
            m_selectedRow = -1; // reset selection when switching modes

        // Warn if two attached pairs active simultaneously
        int attachedPairs = 0;
        for (int i = 1; i < 8; i += 2)
            if (m_decoded[i].attached)
                attachedPairs++;
        if (attachedPairs > 1) {
            ImGui::SameLine(0, 16);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                               "\xe2\x9a\xa0"
                               " %d attached pairs active (only one supported)",
                               attachedPairs);
        }
    } else {
        ImGui::TextDisabled("SPREN: ---");
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("Active: ---");
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("Mux: ---");
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // --- Layout ---
    // Top region: [table] | vertical splitter | [Scanlines histogram]
    // Optional bottom region (when a row is selected): horizontal splitter + detail panel
    const float kSplit = 6.0f;
    const float availX = ImGui::GetContentRegionAvail().x;
    const float availY = ImGui::GetContentRegionAvail().y;

    bool showDetail = (m_selectedRow >= 0 && running && m_cachedRegsValid);

    // Resizable bottom detail panel height (clamped to keep a usable top region)
    float topH = availY;
    if (showDetail) {
        const float maxDetail = availY - 80.0f - kSplit;
        if (m_detailPanelH > maxDetail)
            m_detailPanelH = maxDetail;
        if (m_detailPanelH < 80.0f)
            m_detailPanelH = 80.0f;
        topH = availY - m_detailPanelH - kSplit;
    }

    // Resizable Scanlines panel width (clamped to keep a usable table region)
    const float maxHist = availX - 160.0f - kSplit;
    if (m_histPanelW > maxHist)
        m_histPanelW = maxHist;
    if (m_histPanelW < 50.0f)
        m_histPanelW = 50.0f;
    const float histW = m_histPanelW;
    const float tableW = availX - histW - kSplit;

    // Table
    ImGui::BeginChild("spr_table_area", ImVec2(tableW, topH), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::BeginDisabled(!running);
    if (m_traceMode)
        DrawTraceTable(running && m_cachedRegsValid ? m_cachedRegs : core::Custom{});
    else
        DrawTable(running && m_cachedRegsValid ? m_cachedRegs : core::Custom{});
    ImGui::EndDisabled();
    ImGui::EndChild();

    // Vertical splitter (drag right grows the table, shrinks Scanlines)
    ImGui::SameLine(0, 0);
    m_histPanelW -= SpritesSplitter("##spr_vsplit", true, topH, kSplit);
    ImGui::SameLine(0, 0);

    // Scanlines histogram
    ImGui::BeginChild("spr_hist_area", ImVec2(histW, topH), false);
    DrawHistogram();
    ImGui::EndChild();

    // --- Detail panel ---
    if (showDetail) {
        // Horizontal splitter (drag down grows the table region, shrinks the detail panel)
        m_detailPanelH -= SpritesSplitter("##spr_hsplit", false, availX, kSplit);

        // Resolve the selected row to a channel: in trace mode it's a flat instance row, in
        // standard mode it's the channel index directly.
        int detailChannel = -1;
        if (m_traceMode) {
            if (m_selectedRow >= 0 && m_selectedRow < static_cast<int>(m_traceRows.size()))
                detailChannel = m_traceRows[static_cast<size_t>(m_selectedRow)].spriteIdx;
        } else {
            detailChannel = m_selectedRow;
        }

        ImGui::BeginChild("spr_detail", ImVec2(0, m_detailPanelH), false);
        if (detailChannel >= 0 && detailChannel < 8)
            DrawInstanceDetail(detailChannel, m_cachedRegs);
        ImGui::EndChild();
    }
}

void SpritesView::DrawTable(const core::Custom& regs)
{
    bool sprenEnabled = (regs.dmaconr & (1 << 5)) != 0;

    static const ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV |
                                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_SizingFixedFit | kAdjustableColumns;

    if (!ImGui::BeginTable("spr_tbl", 11, tableFlags))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("BBox", ImGuiTableColumnFlags_WidthFixed, 28.0f);
    ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 36.0f);
    ImGui::TableSetupColumn("HSTRT", ImGuiTableColumnFlags_WidthFixed, 44.0f);
    ImGui::TableSetupColumn("VSTRT", ImGuiTableColumnFlags_WidthFixed, 44.0f);
    ImGui::TableSetupColumn("VSTOP", ImGuiTableColumnFlags_WidthFixed, 44.0f);
    ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("DMA", ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("Pointer", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    // Keep the lone stretch column last: a fixed column placed after a stretch column makes
    // ImGui's resize redistribution jump unpredictably on every trailing column.
    ImGui::TableSetupColumn("Colors", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (int i = 0; i < 8; i++) {
        auto& s = m_decoded[i];
        ImGui::PushID(i);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, 34.0f);

        // Ch column — row select, coloured with per-sprite colour (same as trace mode).
        // Drawn first (before the checkboxes in columns 0/1) so the span-all-columns
        // Selectable sits beneath them and the checkboxes stay clickable.
        ImGui::TableSetColumnIndex(2);
        bool selected = (m_selectedRow == i);
        {
            ImU32 sprCol = kSpriteColors[i];
            ImVec4 sprColF = {static_cast<float>((sprCol >> 0) & 0xFF) / 255.0f,
                              static_cast<float>((sprCol >> 8) & 0xFF) / 255.0f,
                              static_cast<float>((sprCol >> 16) & 0xFF) / 255.0f, 1.0f};
            ImGui::PushStyleColor(ImGuiCol_Text, sprColF);
            char chLabel[4];
            snprintf(chLabel, sizeof(chLabel), "%d", i);
            if (ImGui::Selectable(chLabel, selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, 32.0f))) {
                m_selectedRow = selected ? -1 : i;
                m_detailInstance = 0; // open the detail panel on this channel's first instance
            }
            ImGui::PopStyleColor();
            // Hovering the row previews this channel's bounding boxes (all instances).
            if (ImGui::IsItemHovered()) {
                m_hoverChannel = i;
                m_hoverInstance = -1;
            }
        }

        // Enable checkbox — first column (synced with the Display window's sprite mask)
        ImGui::TableSetColumnIndex(0);
        DrawSpriteEnableCheckbox(i);

        // BBox checkbox — second column
        ImGui::TableSetColumnIndex(1);
        {
            bool bboxOn = (m_bboxMask & (1 << i)) != 0;
            if (ImGui::Checkbox("##bb", &bboxOn)) {
                if (bboxOn)
                    m_bboxMask |= static_cast<uint8_t>(1 << i);
                else
                    m_bboxMask &= static_cast<uint8_t>(~(1 << i));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show bounding box overlay on emulator screen");
        }

        // Preview column
        ImGui::TableSetColumnIndex(3);
        if (s.attached) {
            // Odd attached sprite: show arrow to even partner
            ImGui::TextDisabled("\xe2\x86\x91%d", i - 1);
        } else if (s.texId && s.texH > 0) {
            const float cellH = 30.0f;
            const float scale = std::min(cellH / static_cast<float>(s.texH), 4.0f);
            const float dW = std::max(static_cast<float>(s.texW) * scale, 4.0f);
            const float dH = std::min(static_cast<float>(s.texH) * scale, cellH);
            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
            ImGui::Image((ImTextureID)(uintptr_t)s.texId, ImVec2(dW, dH));
            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
        } else {
            ImGui::TextDisabled("---");
        }

        // HSTART
        ImGui::TableSetColumnIndex(4);
        if (s.height > 0 || s.ptr != 0)
            ImGui::Text("%d", s.hstart);
        else
            ImGui::TextDisabled("---");

        // VSTART
        ImGui::TableSetColumnIndex(5);
        if (s.height > 0 || s.ptr != 0)
            ImGui::Text("%d", s.vstart);
        else
            ImGui::TextDisabled("---");

        // VSTOP
        ImGui::TableSetColumnIndex(6);
        if (s.height > 0 || s.ptr != 0)
            ImGui::Text("%d", s.vstop);
        else
            ImGui::TextDisabled("---");

        // Mode
        ImGui::TableSetColumnIndex(7);
        if (s.attached) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "ATTACHED");
        } else if (s.isEvenOfPair) {
            ImGui::TextColored(ImVec4(0.8f, 0.9f, 0.3f, 1.0f), "15-colour");
        } else {
            ImGui::TextDisabled("3-colour");
        }

        // Colors column — small swatches
        ImGui::TableSetColumnIndex(10);
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            const float sw = 8.0f, sh = 12.0f, gap = 1.0f;
            int nc = s.numColors;
            for (int c = 1; c <= nc && c < 16; c++) {
                ImVec2 min = ImVec2(pos.x + (c - 1) * (sw + gap), pos.y + 1.0f);
                ImVec2 max = ImVec2(min.x + sw, min.y + sh);
                dl->AddRectFilled(min, max, s.palette[c]);
                dl->AddRect(min, max, IM_COL32(80, 80, 80, 200));
            }
            // Transparent swatch
            if (!s.attached) {
                ImVec2 min = ImVec2(pos.x + nc * (sw + gap), pos.y + 1.0f);
                ImVec2 max = ImVec2(min.x + sw, min.y + sh);
                // Checkerboard for transparent
                dl->AddRectFilled(min, max, IM_COL32(60, 60, 60, 255));
                dl->AddLine(ImVec2(min.x, min.y), ImVec2(max.x, max.y),
                            IM_COL32(180, 180, 180, 180));
                dl->AddRect(min, max, IM_COL32(80, 80, 80, 200));
            }
            ImGui::Dummy(ImVec2((nc + 1) * (sw + gap), sh + 2.0f));
        }

        // DMA active LED
        ImGui::TableSetColumnIndex(8);
        {
            ImVec4 ledCol = (s.active && sprenEnabled) ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                                                       : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
            ImGui::TextColored(ledCol, "\xe2\x97\x8f"); // ● U+25CF
        }

        // Pointer — clickable memory-address hyperlink
        ImGui::TableSetColumnIndex(9);
        if (!s.attached)
            DrawAddressLink(s.ptr);
        else
            ImGui::TextDisabled("---");

        ImGui::PopID();
    }

    ImGui::EndTable();
}

void SpritesView::DrawInstanceDetail(int channel, const core::Custom& regs)
{
    if (channel < 0 || channel > 7)
        return;

    const auto& s = m_decoded[channel];
    const int instCount = static_cast<int>(s.instances.size());

    // Header line (spans the full width above the three panes).
    ImU32 sprCol = kSpriteColors[channel];
    ImVec4 sprColF = {static_cast<float>((sprCol >> 0) & 0xFF) / 255.0f,
                      static_cast<float>((sprCol >> 8) & 0xFF) / 255.0f,
                      static_cast<float>((sprCol >> 16) & 0xFF) / 255.0f, 1.0f};
    ImGui::TextColored(sprColF, "SPR%d", channel);
    ImGui::SameLine(0, 10);
    // pos/ctl/ptr vary per instance, so they live in the middle pane — the header is just the
    // channel and how many instances it produced this frame.
    ImGui::TextDisabled("%d instance%s this frame", instCount, instCount == 1 ? "" : "s");
    ImGui::Separator();

    if (instCount == 0) {
        ImGui::TextDisabled("No instances this frame.");
        return;
    }

    // Keep the selected instance in range (selection can outlive a re-decode).
    if (m_detailInstance >= instCount)
        m_detailInstance = instCount - 1;
    if (m_detailInstance < 0)
        m_detailInstance = 0;

    // --- 3-pane layout: [instance list] | vsplit | [breakdown] | vsplit | [preview + palette] ---
    const float kSplit = 6.0f;
    const float availX = ImGui::GetContentRegionAvail().x;
    const float availY = ImGui::GetContentRegionAvail().y;

    // Clamp the resizable side-pane widths, keeping the middle usable.
    float leftW = std::max(80.0f, std::min(m_detailLeftW, availX * 0.4f));
    float rightW = std::max(120.0f, std::min(m_detailRightW, availX * 0.55f));
    if (leftW + rightW > availX - 120.0f) {
        rightW = std::max(120.0f, availX - 120.0f - leftW);
        if (leftW + rightW > availX - 80.0f)
            leftW = std::max(80.0f, availX - 80.0f - rightW);
    }
    const float midW = std::max(60.0f, availX - leftW - rightW - 2.0f * kSplit);

    // ---- Left pane: instance list ----
    ImGui::BeginChild("spr_det_left", ImVec2(leftW, availY), true);
    ImGui::TextDisabled("Instances");
    ImGui::Separator();
    for (int k = 0; k < instCount; k++) {
        const auto& inst = s.instances[static_cast<size_t>(k)];
        char label[48];
        snprintf(label, sizeof(label), "#%d  v%d-%d", k, inst.vstart, inst.vstop);
        if (ImGui::Selectable(label, m_detailInstance == k))
            m_detailInstance = k;
        if (inst.vpos_armed > 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip("Armed at vpos %d  ptr=$%06X", inst.vpos_armed, inst.ptr);
    }
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    m_detailLeftW = leftW + SpritesSplitter("##det_lsplit", true, availY, kSplit);
    ImGui::SameLine(0, 0);

    // The instance the middle/right panes describe.
    const auto& inst = s.instances[static_cast<size_t>(m_detailInstance)];
    const int height = inst.vstop - inst.vstart;

    // Per-instance register bits, reconstructed from this instance's decoded geometry.
    const int sv8 = (inst.vstart >> 8) & 1;
    const int ev8 = (inst.vstop >> 8) & 1;
    const int sh0 = inst.hstart & 1;
    const int att = s.attached ? 1 : 0;
    const uint16_t posWord =
        static_cast<uint16_t>(((inst.vstart & 0xFF) << 8) | ((inst.hstart >> 1) & 0xFF));
    const uint16_t ctlWord = static_cast<uint16_t>(((inst.vstop & 0xFF) << 8) | (att << 7) |
                                                   (sv8 << 2) | (ev8 << 1) | sh0);

    // ---- Middle pane: full breakdown ----
    ImGui::BeginChild("spr_det_mid", ImVec2(midW, availY), false);
    ImGui::Indent(8.0f); // slight left margin (borderless child sits flush otherwise)
    {
        // Section 1: instance identity — index, hyperlinked pointer, per-instance pos/ctl.
        ImGui::Text("Instance %d / %d", m_detailInstance, instCount - 1);
        ImGui::SameLine(0, 14);
        ImGui::TextDisabled("ptr");
        ImGui::SameLine(0, 4);
        DrawAddressLink(inst.ptr);
        if (inst.vpos_armed > 0) {
            ImGui::SameLine(0, 14);
            ImGui::TextDisabled("armed vpos %d", inst.vpos_armed);
        }
        ImGui::Spacing();
        ImGui::Text("pos=$%04X", posWord);
        ImGui::SameLine(0, 16);
        ImGui::Text("ctl=$%04X", ctlWord);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Section 2: raw POS/CTL bit fields + decoded position summary, side by side.
        struct BitField
        {
            const char* name;
            int value;
            bool highlight; // amber for ATT bit on an odd channel
        };
        BitField fields[] = {
            {"VSTRT[7:0]", inst.vstart & 0xFF, false},
            {"SH[8:1]", (inst.hstart >> 1) & 0xFF, false},
            {"VSTOP[7:0]", inst.vstop & 0xFF, false},
            {"ATT", att, (channel & 1) != 0},
            {"SV8", sv8, false},
            {"EV8", ev8, false},
            {"SH0", sh0, false},
        };
        if (ImGui::BeginTable("spr_bits", 2,
                              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable,
                              ImVec2(200, 0))) {
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& bf : fields) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (bf.highlight && bf.value)
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s", bf.name);
                else
                    ImGui::Text("%s", bf.name);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d ($%X)", bf.value, bf.value);
            }
            ImGui::EndTable();
        }

        ImGui::SameLine(0, 24);
        ImGui::BeginGroup();
        ImGui::Text("HSTART: %d (lores px)", inst.hstart);
        ImGui::Text("VSTART: %d", inst.vstart);
        ImGui::Text("VSTOP:  %d", inst.vstop);
        ImGui::Text("Height: %d lines", height > 0 ? height : 0);
        ImGui::Text("Color base: COLOR[%d..%d]", s.colorBase + 1, s.colorBase + s.numColors);
        ImGui::EndGroup();

        ImGui::Spacing();

        // Section 3: warnings / mode notes.
        bool sprDmaEnabled = (regs.dmaconr & (1 << 5)) != 0;
        if (!sprDmaEnabled)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "\xe2\x9a\xa0"
                                                               " Sprite DMA disabled (SPREN=0)");
        if (height <= 0)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "\xe2\x9a\xa0"
                               " Invisible sprite (vstop <= vstart)");
        if (s.attached) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.5f, 1.0f), "Odd sprite of attached pair");
            ImGui::TextDisabled("Preview rendered on the even partner (SPR%d).", channel - 1);
        } else if (s.isEvenOfPair) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.5f, 1.0f),
                               "Even sprite of attached pair (15-colour mode)");
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    m_detailRightW = rightW - SpritesSplitter("##det_rsplit", true, availY, kSplit);
    ImGui::SameLine(0, 0);

    // ---- Right pane: enlarged preview + palette ----
    ImGui::BeginChild("spr_det_right", ImVec2(rightW, availY), false);
    ImGui::Indent(8.0f); // slight left margin (borderless child sits flush otherwise)
    {
        ImGui::TextDisabled("Preview");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##zoom", &m_previewZoom, 1.0f, 16.0f, "%.0fx");

        // Reserve the bottom strip for the palette; the preview gets the rest (scrollable).
        const float kPaletteH = 44.0f;
        float prevH = std::max(40.0f, ImGui::GetContentRegionAvail().y - kPaletteH);

        ImGui::BeginChild("spr_det_prev", ImVec2(0, prevH), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        if (inst.texId && inst.texH > 0) {
            float dW = static_cast<float>(inst.texW) * m_previewZoom;
            float dH = static_cast<float>(inst.texH) * m_previewZoom;
            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
            ImGui::Image((ImTextureID)(uintptr_t)inst.texId, ImVec2(dW, dH));
            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
        } else {
            ImGui::TextDisabled(s.attached ? "(shown on even partner)" : "(no image)");
        }
        ImGui::EndChild();

        // Palette swatches — prefer the per-instance snapshot captured at arm time.
        const ImU32* pal = inst.paletteValid ? inst.palette : s.palette;
        ImGui::TextDisabled("%s", inst.paletteValid ? "Palette (arm time)" : "Palette (end-frame)");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        const float sw = 14.0f, sh = 18.0f, gap = 2.0f;
        int nc = s.numColors;
        for (int c = 1; c <= nc && c < 16; c++) {
            ImVec2 mn = ImVec2(pos.x + static_cast<float>(c - 1) * (sw + gap), pos.y);
            ImVec2 mx = ImVec2(mn.x + sw, mn.y + sh);
            dl->AddRectFilled(mn, mx, pal[c]);
            dl->AddRect(mn, mx, IM_COL32(120, 120, 120, 200));
        }
        ImGui::Dummy(ImVec2(static_cast<float>(nc) * (sw + gap), sh + 2.0f));
    }
    ImGui::EndChild();
}

void SpritesView::DrawHistogram()
{
    if (m_histDirty)
        RebuildHistogram();

    ImGui::Text("Scanlines");
    if (m_histTexId) {
        // Scale the histogram texture to fill the (resizable) panel. The texture is
        // kHistW × kHistH; map all overlay/hover math through colW (per-sprite column width)
        // and scaleY (scanline → screen px) so the splitter can resize the panel freely.
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float dispW = std::max(avail.x, 16.0f);
        float dispH = std::max(avail.y, 16.0f);
        ImGui::Image((ImTextureID)(uintptr_t)m_histTexId, ImVec2(dispW, dispH));

        ImVec2 imgMin = ImGui::GetItemRectMin();
        ImVec2 imgMax = ImGui::GetItemRectMax();
        const float colW = dispW / 8.0f;                         // per-sprite column width
        const float fillW = colW * 0.9f;                         // matches the 9/10 texture fill
        const float scaleY = dispH / static_cast<float>(kHistH); // scanline → screen px

        // Hover tooltip
        if (ImGui::IsItemHovered()) {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            int scanline = static_cast<int>((mouse.y - imgMin.y) / scaleY);
            int sprCol = static_cast<int>((mouse.x - imgMin.x) / colW);
            if (scanline >= 0 && scanline < kHistH && sprCol >= 0 && sprCol < 8)
                ImGui::SetTooltip("SPR%d  scanline %d", sprCol, scanline);
        }

        // Selection highlight overlay
        if (m_selectedRow >= 0) {
            ImDrawList* dl = ImGui::GetWindowDrawList();

            if (!m_traceMode) {
                // Default mode: highlight the selected sprite's column and all its instances
                int si = m_selectedRow;
                if (si >= 0 && si < 8) {
                    float cx = imgMin.x + static_cast<float>(si) * colW;
                    // Dim all other columns slightly
                    for (int j = 0; j < 8; j++) {
                        if (j == si)
                            continue;
                        float jx = imgMin.x + static_cast<float>(j) * colW;
                        dl->AddRectFilled({jx, imgMin.y}, {jx + fillW, imgMax.y},
                                          IM_COL32(0, 0, 0, 100));
                    }
                    // Bright border around the selected column
                    dl->AddRect({cx, imgMin.y}, {cx + fillW, imgMax.y},
                                IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.5f);
                    // White glow over each instance's scanline range
                    for (const auto& inst : m_decoded[si].instances) {
                        float y0 = imgMin.y + static_cast<float>(inst.vstart) * scaleY;
                        float y1 = imgMin.y + static_cast<float>(inst.vstop) * scaleY;
                        dl->AddRectFilled({cx, y0}, {cx + fillW, y1}, IM_COL32(255, 255, 255, 80));
                    }
                }
            } else {
                // Trace mode: highlight the specific instance's scanline range
                if (m_selectedRow < static_cast<int>(m_traceRows.size())) {
                    auto [si, ki] = m_traceRows[static_cast<size_t>(m_selectedRow)];
                    const auto& inst = m_decoded[si].instances[static_cast<size_t>(ki)];
                    float cx = imgMin.x + static_cast<float>(si) * colW;
                    float y0 = imgMin.y + static_cast<float>(inst.vstart) * scaleY;
                    float y1 = imgMin.y + static_cast<float>(inst.vstop) * scaleY;
                    // Dim other columns
                    for (int j = 0; j < 8; j++) {
                        if (j == si)
                            continue;
                        float jx = imgMin.x + static_cast<float>(j) * colW;
                        dl->AddRectFilled({jx, imgMin.y}, {jx + fillW, imgMax.y},
                                          IM_COL32(0, 0, 0, 100));
                    }
                    // Bright filled highlight + border at the specific scanline range
                    dl->AddRectFilled({cx, y0}, {cx + fillW, y1}, IM_COL32(255, 255, 255, 100));
                    dl->AddRect({cx, y0}, {cx + fillW, y1}, IM_COL32(255, 255, 255, 255), 0.0f, 0,
                                2.0f);
                }
            }
        }
    } else {
        ImGui::TextDisabled("(waiting)");
    }
}

// ----- trace mode table ------------------------------------------------------

void SpritesView::DrawTraceTable(const core::Custom& regs)
{
    bool sprenEnabled = (regs.dmaconr & (1 << 5)) != 0;

    if (m_traceRows.empty()) {
        ImGui::TextDisabled("No sprite instances detected this frame.");
        ImGui::TextDisabled("Enable sprite DMA and ensure the DLL patch is active.");
        return;
    }

    static const ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV |
                                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_SizingFixedFit | kAdjustableColumns;

    if (!ImGui::BeginTable("spr_trace_tbl", 10, tableFlags))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("BBox", ImGuiTableColumnFlags_WidthFixed, 28.0f);
    ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("Armed", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 36.0f);
    ImGui::TableSetupColumn("HSTRT", ImGuiTableColumnFlags_WidthFixed, 44.0f);
    ImGui::TableSetupColumn("VSTRT", ImGuiTableColumnFlags_WidthFixed, 44.0f);
    ImGui::TableSetupColumn("VSTOP", ImGuiTableColumnFlags_WidthFixed, 44.0f);
    ImGui::TableSetupColumn("Pointer", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    // Stretch column kept last so trailing-column resizing stays predictable (see DrawTable).
    ImGui::TableSetupColumn("Colors", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (int row = 0; row < static_cast<int>(m_traceRows.size()); row++) {
        int si = m_traceRows[static_cast<size_t>(row)].spriteIdx;
        int ki = m_traceRows[static_cast<size_t>(row)].instIdx;
        const auto& s = m_decoded[si];
        const auto& inst = s.instances[static_cast<size_t>(ki)];

        ImGui::PushID(row);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, 34.0f);

        // Ch column — selectable row, coloured with per-sprite colour. Drawn first (before the
        // checkboxes in columns 0/1) so the span-all-columns Selectable stays beneath them.
        ImGui::TableSetColumnIndex(2);
        bool selected = (m_selectedRow == row);
        ImU32 sprCol = kSpriteColors[si];
        ImVec4 sprColF = {static_cast<float>((sprCol >> 0) & 0xFF) / 255.0f,
                          static_cast<float>((sprCol >> 8) & 0xFF) / 255.0f,
                          static_cast<float>((sprCol >> 16) & 0xFF) / 255.0f, 1.0f};
        ImGui::PushStyleColor(ImGuiCol_Text, sprColF);
        char chLabel[4];
        snprintf(chLabel, sizeof(chLabel), "%d", si);
        if (ImGui::Selectable(chLabel, selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, 32.0f))) {
            m_selectedRow = selected ? -1 : row;
            m_detailInstance = ki; // open the detail panel pre-selecting the clicked instance
        }
        ImGui::PopStyleColor();
        // Hovering the row previews this specific instance's bounding box.
        if (ImGui::IsItemHovered()) {
            m_hoverChannel = si;
            m_hoverInstance = ki;
        }

        // Enable checkbox — first column (synced with the Display window's sprite mask)
        ImGui::TableSetColumnIndex(0);
        DrawSpriteEnableCheckbox(si);

        // BBox checkbox — second column (per-channel m_bboxMask bit, same as default mode)
        ImGui::TableSetColumnIndex(1);
        {
            bool bboxOn = (m_bboxMask & (1 << si)) != 0;
            if (ImGui::Checkbox("##bb", &bboxOn)) {
                if (bboxOn)
                    m_bboxMask |= static_cast<uint8_t>(1 << si);
                else
                    m_bboxMask &= static_cast<uint8_t>(~(1 << si));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show bounding box overlay for SPR%d", si);
        }

        // Armed column (vpos when SPRxPT was written)
        ImGui::TableSetColumnIndex(3);
        if (inst.vpos_armed > 0)
            ImGui::Text("%d", inst.vpos_armed);
        else
            ImGui::TextDisabled("---");

        // Preview column
        ImGui::TableSetColumnIndex(4);
        if (s.attached) {
            ImGui::TextDisabled("ATT");
        } else if (inst.texId && inst.texH > 0) {
            const float cellH = 30.0f;
            const float scale = std::min(cellH / static_cast<float>(inst.texH), 4.0f);
            const float dW = std::max(static_cast<float>(inst.texW) * scale, 4.0f);
            const float dH = std::min(static_cast<float>(inst.texH) * scale, cellH);
            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
            ImGui::Image((ImTextureID)(uintptr_t)inst.texId, ImVec2(dW, dH));
            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
        } else {
            ImGui::TextDisabled("---");
        }

        // HSTRT
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("%d", inst.hstart);

        // VSTRT
        ImGui::TableSetColumnIndex(6);
        ImGui::Text("%d", inst.vstart);

        // VSTOP
        ImGui::TableSetColumnIndex(7);
        ImGui::Text("%d", inst.vstop);

        // Colors column — use per-instance palette snapshot when available
        ImGui::TableSetColumnIndex(9);
        {
            // Prefer the instance's own palette (captured at arm time); fall back to channel
            // palette
            const ImU32* pal = inst.paletteValid ? inst.palette : s.palette;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            const float sw = 8.0f, sh = 12.0f, gap = 1.0f;
            int nc = s.numColors;
            for (int c = 1; c <= nc && c < 16; c++) {
                ImVec2 min = ImVec2(pos.x + static_cast<float>(c - 1) * (sw + gap), pos.y + 1.0f);
                ImVec2 max = ImVec2(min.x + sw, min.y + sh);
                dl->AddRectFilled(min, max, pal[c]);
                dl->AddRect(min, max, IM_COL32(80, 80, 80, 200));
            }
            // Transparent swatch
            if (!s.attached) {
                ImVec2 min = ImVec2(pos.x + static_cast<float>(nc) * (sw + gap), pos.y + 1.0f);
                ImVec2 max = ImVec2(min.x + sw, min.y + sh);
                dl->AddRectFilled(min, max, IM_COL32(60, 60, 60, 255));
                dl->AddLine(ImVec2(min.x, min.y), ImVec2(max.x, max.y),
                            IM_COL32(180, 180, 180, 180));
                dl->AddRect(min, max, IM_COL32(80, 80, 80, 200));
            }
            ImGui::Dummy(ImVec2(static_cast<float>(nc + 1) * (sw + gap), sh + 2.0f));
        }

        // Pointer — clickable memory-address hyperlink
        ImGui::TableSetColumnIndex(8);
        if (!s.attached)
            DrawAddressLink(inst.ptr);
        else
            ImGui::TextDisabled("---");

        ImGui::PopID();
    }

    ImGui::EndTable();
    (void)sprenEnabled;
}

// ----- overlay ---------------------------------------------------------------

/*static*/ void SpritesView::DrawDashedRect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col,
                                            float thickness, float dashLen)
{
    auto dashLine = [&](ImVec2 a, ImVec2 b) {
        float dx = b.x - a.x;
        float dy = b.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.001f)
            return;
        float ux = dx / len, uy = dy / len;
        float t = 0.0f;
        bool on = true;
        while (t < len) {
            float t2 = std::min(t + dashLen, len);
            if (on)
                dl->AddLine({a.x + ux * t, a.y + uy * t}, {a.x + ux * t2, a.y + uy * t2}, col,
                            thickness);
            t = t2 + dashLen;
            on = !on;
        }
    };
    dashLine({mn.x, mn.y}, {mx.x, mn.y}); // top
    dashLine({mx.x, mn.y}, {mx.x, mx.y}); // right
    dashLine({mx.x, mx.y}, {mn.x, mx.y}); // bottom
    dashLine({mn.x, mx.y}, {mn.x, mn.y}); // left
}

void SpritesView::DrawOverlay(ImDrawList* dl, ImVec2 screenMin, ImVec2 screenMax)
{
    // Consume the row-hover preview state set by the last table draw. Resetting it here (rather
    // than in Draw()) auto-clears the hover overlay when the Sprites window is closed, since the
    // table — the only place that sets it — no longer runs.
    int hoverChannel = m_hoverChannel;
    int hoverInstance = m_hoverInstance;
    m_hoverChannel = -1;
    m_hoverInstance = -1;

    if (m_bboxMask == 0 && hoverChannel < 0)
        return;

    EnsureDecoded();

    if (!m_cachedRegsValid || !m_emulatorView || !m_emulatorView->IsRunning())
        return;

    // --- Coordinate mapping ---
    // Use hardware frame geometry to correctly map Amiga CCK coordinates onto the
    // (potentially cropped) texture that is displayed on screen.
    auto geom = m_emulatorView->GetEmulator()->GetDisplayGeometry();
    float texWidth = static_cast<float>(m_emulatorView->GetEmulator()->GetWidth());
    float texHeight = static_cast<float>(m_emulatorView->GetEmulator()->GetHeight());

    // Use the shared mapping rather than re-deriving it here, so the sprite boxes stay aligned
    // with the DIW/DDF geometry and blit-destination overlays — including the resolutions
    // (super-hires) this view's own copy of the math used to get wrong.
    const core::AmigaScreenMap smap =
        core::AmigaScreenMap::Build(geom, static_cast<int>(texWidth), static_cast<int>(texHeight));
    if (!smap.valid || smap.loresTexW <= 0.0f || smap.scanTexH <= 0.0f)
        return;

    float loresToScreen = (screenMax.x - screenMin.x) / smap.loresTexW;
    float scanToScreen = (screenMax.y - screenMin.y) / smap.scanTexH;

    // hbase / vbase: the Amiga hardware coordinate that maps to the top-left corner of the
    // displayed texture.
    int hbase = smap.hbase;
    int vbase = smap.vbase;

    auto amigaToScreen = [&](int hpix, int vscan) -> ImVec2 {
        return {screenMin.x + static_cast<float>(hpix - hbase) * loresToScreen,
                screenMin.y + static_cast<float>(vscan - vbase) * scanToScreen};
    };

    for (int i = 0; i < 8; i++) {
        bool maskOn = (m_bboxMask & (1 << i)) != 0;
        bool hoverOn = (hoverChannel == i);
        if (!maskOn && !hoverOn)
            continue;

        // Skip the odd sprite of an attached pair; boxes are drawn by the even sprite
        if (m_decoded[i].attached)
            continue;

        const auto& s = m_decoded[i];
        ImU32 col = kSpriteColors[i];

        for (int k = 0; k < static_cast<int>(s.instances.size()); k++) {
            // Trace-mode hover targets a single instance; restrict to it unless the BBox
            // checkbox is forcing every instance on for this channel.
            if (!maskOn && hoverOn && hoverInstance >= 0 && k != hoverInstance)
                continue;

            const auto& inst = s.instances[k];
            if (inst.vstop <= inst.vstart)
                continue;

            // Sprite is 16 lores pixels wide; hstart is in lores pixel columns
            ImVec2 topLeft = amigaToScreen(inst.hstart, inst.vstart);
            ImVec2 botRight = amigaToScreen(inst.hstart + 16, inst.vstop);

            // Clip check
            if (botRight.x < screenMin.x || topLeft.x > screenMax.x || botRight.y < screenMin.y ||
                topLeft.y > screenMax.y)
                continue;

            float thickness = (k == 0) ? 1.5f : 1.0f;
            if (k == 0) {
                dl->AddRect(topLeft, botRight, col, 0.0f, 0, thickness);
            } else {
                DrawDashedRect(dl, topLeft, botRight, col, thickness, 4.0f);
            }

            // Label
            char label[24];
            if (k == 0)
                snprintf(label, sizeof(label), "SPR%d", i);
            else
                snprintf(label, sizeof(label), "SPR%d \xc3\x97%d", i, k + 1); // "×N"

            ImVec2 labelPos = {topLeft.x + 2.0f, topLeft.y + 1.0f};
            dl->AddText(labelPos, col, label);

            // Crosshair at hotspot
            if (m_showCrosshairs) {
                float cx = topLeft.x;
                float cy = topLeft.y;
                float arm = 6.0f;
                dl->AddLine({cx - arm, cy}, {cx + arm, cy}, col, 1.0f);
                dl->AddLine({cx, cy - arm}, {cx, cy + arm}, col, 1.0f);
            }
        }
    }
}

} // namespace adm::ui
