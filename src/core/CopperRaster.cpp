#include "CopperRaster.h"

#include <algorithm>
#include <cstring>

#include "Constants.h"

namespace adm::core::copraster {

namespace {

// Mirrors the DMA_EVENT_* bits in PuAE's sources/src/include/debug.h. Only the ones this view
// draws are listed; the record carries many more. These are read, not copied logic — the same
// way DmaStatsView already reads the DMARECORD_* type codes.
constexpr uint32_t kEvtCopperWake = 16;
constexpr uint32_t kEvtCopperWanted = 128;
constexpr uint32_t kEvtLol = 0x00080000;
constexpr uint32_t kEvtDdfStrt = 4096;
constexpr uint32_t kEvtDdfStop = 8192;
constexpr uint32_t kEvtDdfStop2 = 16384;
constexpr uint32_t kEvtHbs = 0x00100000;
constexpr uint32_t kEvtHbe = 0x00200000;
constexpr uint32_t kEvtHdiws = 0x00400000;
constexpr uint32_t kEvtHdiwe = 0x00800000;
constexpr uint32_t kEvtHss = 0x02000000;
constexpr uint32_t kEvtHse = 0x04000000;

constexpr int kDmaTypeMax = 10;

// PuAE records LOL at REFRESH_FIRST_HPOS + 2 (custom.c); a few clocks of margin either side.
constexpr int kLolScanCC = 16;

// Straight src-over composite on 8-bit channels.
uint32_t BlendOver(uint32_t dst, uint32_t src)
{
    const uint32_t sa = AlphaOf(src);
    if (sa == 0)
        return dst;
    if (sa == 255)
        return src;

    const uint32_t da = AlphaOf(dst);
    const uint32_t inv = 255u - sa;
    const uint32_t outA = sa + da * inv / 255u;
    if (outA == 0)
        return 0;

    auto chan = [&](int shift) {
        const uint32_t s = (src >> shift) & 0xFFu;
        const uint32_t d = (dst >> shift) & 0xFFu;
        const uint32_t v = (s * sa + d * da * inv / 255u) / outA;
        return std::min<uint32_t>(255u, v);
    };
    return PackRGBA(chan(0), chan(8), chan(16), outA);
}

// Scales a colour's RGB toward black, keeping its alpha. Used for the blitter's A/B/C/D
// sub-channels and the copper's fetch/wait/special sub-items, which the record distinguishes in
// `extra` but which share one channel colour.
uint32_t Shade(uint32_t c, float f)
{
    auto ch = [&](int shift) {
        const float v = static_cast<float>((c >> shift) & 0xFFu) * f;
        return static_cast<uint32_t>(std::clamp(v, 0.0f, 255.0f));
    };
    return PackRGBA(ch(0), ch(8), ch(16), AlphaOf(c));
}

// Sub-item shading factor for a slot. PuAE stores the sub-channel in `extra`: for the blitter
// that is A/B/C/D then fill and line, and for the copper 0 = fetch, 1 = waiting, 2 = special.
float ExtraShade(int type, uint16_t extra)
{
    if (type == AmigaEmulator::kDmaRecordBlitter)
        return 1.0f - 0.10f * static_cast<float>(std::min<uint16_t>(extra, 6));
    if (type == AmigaEmulator::kDmaRecordCopper) {
        if (extra == 1)
            return 0.70f; // waiting
        if (extra == 2)
            return 0.50f; // special
    }
    return 1.0f;
}

} // namespace

CopClass Classify(uint16_t word1, uint16_t word2)
{
    if ((word1 & 1) != 0)
        return (word2 & 1) ? CopClass::Skip : CopClass::Wait;

    const uint16_t off = word1 & 0x01FEu;
    if (off >= 0x040 && off <= 0x077)
        return CopClass::MoveBlitter;
    if (off >= 0x0E0 && off <= 0x0FF)
        return CopClass::MoveBplPtr;
    if (off >= 0x120 && off <= 0x17F)
        return CopClass::MoveSprite;
    if (off >= 0x180 && off <= 0x1BF)
        return CopClass::MoveColor;
    return CopClass::MoveOther;
}

int DeriveFrameHeight(const AmigaEmulator::DmaSlotRecord* records, int hposCount, int vposCount,
                      int fallback)
{
    if (!records || hposCount <= 0 || vposCount <= 0)
        return fallback;

    const int hLimit = std::min(hposCount, static_cast<int>(constants::beam::CCKPerLine));
    for (int line = vposCount - 1; line >= 0; --line) {
        const auto* row = &records[static_cast<size_t>(line) * hposCount];
        for (int cc = 0; cc < hLimit; ++cc) {
            if (row[cc].type != 0 || row[cc].evt != 0)
                return line + 1;
        }
    }
    return fallback;
}

Palette Palette::Default()
{
    Palette p;
    // Copper instruction classes. ENGINE9000's scheme, except that it draws WAIT and SKIP in one
    // red; they are split here because mistaking a skipped branch for a stalled wait is exactly
    // the confusion this view exists to remove.
    p.copper[static_cast<size_t>(CopClass::Wait)] = PackRGBA(255, 60, 60, 255);
    p.copper[static_cast<size_t>(CopClass::Skip)] = PackRGBA(255, 145, 40, 255);
    p.copper[static_cast<size_t>(CopClass::MoveSprite)] = PackRGBA(240, 240, 240, 255);
    p.copper[static_cast<size_t>(CopClass::MoveBlitter)] = PackRGBA(90, 120, 255, 255);
    p.copper[static_cast<size_t>(CopClass::MoveBplPtr)] = PackRGBA(255, 90, 255, 255);
    p.copper[static_cast<size_t>(CopClass::MoveColor)] = PackRGBA(60, 230, 60, 255);
    p.copper[static_cast<size_t>(CopClass::MoveOther)] = PackRGBA(255, 235, 60, 255);

    p.waitSpan = PackRGBA(170, 40, 40, 110);
    p.copperWanted = PackRGBA(255, 180, 0, 220);
    p.blanking = PackRGBA(0, 0, 0, 110);
    p.sync = PackRGBA(40, 40, 90, 150);
    p.ddfMark = PackRGBA(255, 165, 0, 200);
    p.diwMark = PackRGBA(60, 120, 255, 200);
    return p;
}

void Build(const Input& in, const RasterFrameMap& map, const Palette& pal, const Options& opt,
           Result& out)
{
    out.copperCells = 0;
    out.dmaCells = 0;

    if (!map.valid || map.rasterCC <= 0 || map.rasterLines <= 0) {
        out.width = 0;
        out.height = 0;
        out.lineCC = 0;
        out.rowLineCC.clear();
        out.pixels.clear();
        out.cells.clear();
        return;
    }

    // Always the map's full extent: the overlay stretches this texture over the raster rect the
    // map describes, so a texture a line shorter than the map (a record a line short of the
    // picture) would creep every row out of register. Rows past the record just stay empty.
    const int w = map.rasterCC;
    const int h = map.rasterLines;
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);

    out.width = w;
    out.height = h;
    out.pixels.assign(count, 0u);
    out.cells.assign(count, Cell{});

    const bool haveDma = in.records != nullptr && in.hposCount > 0 && in.vposCount > 0;

    // Clocks in an Agnus line. `w` is wider - see RasterFrameMap::rasterCC - and a row's columns
    // past its line length are filled from the next line by Pass H.
    const int dataW = (map.dataCC > 0 && map.dataCC < w) ? map.dataCC : w;
    out.lineCC = dataW;

    // A long NTSC line has one clock more, and PuAE marks it with LOL near the start of the line.
    out.rowLineCC.assign(static_cast<size_t>(h), static_cast<int16_t>(dataW));
    if (haveDma && dataW < w && dataW < in.hposCount) {
        const int scan = std::min(in.hposCount, kLolScanCC);
        for (int line = 0; line < h && line < in.vposCount; ++line) {
            const auto* row = &in.records[static_cast<size_t>(line) * in.hposCount];
            for (int cc = 0; cc < scan; ++cc) {
                if (row[cc].evt & kEvtLol) {
                    out.rowLineCC[static_cast<size_t>(line)] = static_cast<int16_t>(dataW + 1);
                    break;
                }
            }
        }
    }

    // The record's own slot for a raster cell: bus cycles and the events PuAE timed within the
    // line. Past a row's line length there is no slot of its own - see Pass H.
    auto slotAt = [&](int line, int cc) -> const AmigaEmulator::DmaSlotRecord* {
        if (!haveDma || line >= in.vposCount || cc >= out.LineCCAt(line) || cc >= in.hposCount)
            return nullptr;
        return &in.records[static_cast<size_t>(line) * in.hposCount + cc];
    };
    auto idx = [&](int line, int cc) { return static_cast<size_t>(line) * w + cc; };

    // Lines the frame itself has. The raster can be a line taller (it always covers the
    // picture - see RasterFrameMap::rasterLines), and nothing the Copper did lives down there.
    const int frameLines = (in.vposCount > 0) ? std::min(h, in.vposCount) : h;

    // --- Pass A: blanking and sync bands, painted first so everything else sits on top. -------
    // HBS/HBE and HSS/HSE are recorded per line, and the blank region wraps past the end of the
    // line, so both orderings are handled rather than assuming start < stop.
    if (opt.showBlanking && haveDma) {
        for (int line = 0; line < h; ++line) {
            int hbs = -1, hbe = -1, hss = -1, hse = -1;
            for (int cc = 0; cc < w; ++cc) {
                const auto* r = slotAt(line, cc);
                if (!r || r->evt == 0)
                    continue;
                if (r->evt & kEvtHbs)
                    hbs = cc;
                if (r->evt & kEvtHbe)
                    hbe = cc;
                if (r->evt & kEvtHss)
                    hss = cc;
                if (r->evt & kEvtHse)
                    hse = cc;
            }

            auto shadeRange = [&](int from, int to, uint32_t col) {
                if (from < 0 || to < 0 || col == 0)
                    return;
                int cc = from;
                for (int guard = 0; guard < w; ++guard) {
                    out.pixels[idx(line, cc)] = BlendOver(out.pixels[idx(line, cc)], col);
                    cc = (cc + 1) % w;
                    if (cc == to)
                        break;
                }
            };
            shadeRange(hbs, hbe, pal.blanking);
            shadeRange(hss, hse, pal.sync);
        }
    }

    // --- Pass B: the DMA channels. ------------------------------------------------------------
    if (opt.channelMask != 0 && haveDma) {
        for (int line = 0; line < h; ++line) {
            for (int cc = 0; cc < w; ++cc) {
                const auto* r = slotAt(line, cc);
                if (!r)
                    continue;
                const int type = static_cast<int>(r->type);
                if (type <= 0 || type >= kDmaTypeMax)
                    continue;
                if ((opt.channelMask & (1u << type)) == 0)
                    continue;

                const size_t i = idx(line, cc);
                out.cells[i].dmaSlot =
                    static_cast<int32_t>(static_cast<size_t>(line) * in.hposCount + cc);
                out.cells[i].dmaType = static_cast<uint8_t>(type);

                const uint32_t col = Shade(pal.channel[type], ExtraShade(type, r->extra));
                out.pixels[i] = BlendOver(out.pixels[i], col);
                ++out.dmaCells;
            }
        }
    }

    // --- Pass C: WAIT stall runs. --------------------------------------------------------------
    // A WAIT occupies one slot in the trace but blocks the Copper until the beam reaches its
    // target. The record marks the resume with DMA_EVENT_COPPERWAKE, so the run between the two
    // is the stall — the horizontal bar a one-dimensional trace can never show. The scan is
    // bounded by the next Copper event, which keeps the total work linear in the frame.
    if (opt.showCopper && opt.showWaitSpans && haveDma && in.copperEvents) {
        constexpr uint32_t kCCKPerLine = constants::beam::CCKPerLine;
        for (int e = 0; e < in.copperEventCount; ++e) {
            const auto& ev = in.copperEvents[e];
            if (Classify(ev.word1, ev.word2) != CopClass::Wait)
                continue;

            const uint32_t startL = static_cast<uint32_t>(ev.vpos) * kCCKPerLine + ev.hpos;
            uint32_t limitL = static_cast<uint32_t>(frameLines) * kCCKPerLine;
            if (e + 1 < in.copperEventCount) {
                const auto& nx = in.copperEvents[e + 1];
                limitL = std::min<uint32_t>(limitL, static_cast<uint32_t>(nx.vpos) * kCCKPerLine +
                                                        nx.hpos + 1u);
            }

            for (uint32_t l = startL + 1; l < limitL; ++l) {
                const int line = static_cast<int>(l / kCCKPerLine);
                const int cc = static_cast<int>(l % kCCKPerLine);
                if (line >= frameLines)
                    break;
                // The L-Ref stride is 228; a 227-clock line has no clock 227 to stall on.
                if (cc >= out.LineCCAt(line))
                    continue;
                const size_t i = idx(line, cc);
                out.cells[i].waitBlocked = true;
                out.pixels[i] = BlendOver(out.pixels[i], pal.waitSpan);

                const auto* r = slotAt(line, cc);
                if (r && (r->evt & kEvtCopperWake))
                    break;
            }
        }
    }

    // --- Pass D: the Copper instructions themselves, on top of their own stall runs. -----------
    if (opt.showCopper && in.copperEvents) {
        for (int e = 0; e < in.copperEventCount; ++e) {
            const auto& ev = in.copperEvents[e];
            const int line = static_cast<int>(ev.vpos);
            const int cc = static_cast<int>(ev.hpos);
            // Placed on its own row only; Pass H repeats it in the row above's wrap columns.
            if (line < 0 || line >= frameLines || cc < 0 || cc >= out.LineCCAt(line))
                continue;

            const size_t i = idx(line, cc);
            const CopClass klass = Classify(ev.word1, ev.word2);
            out.cells[i].copperEvent = e;
            out.cells[i].copClass = klass;
            out.pixels[i] = BlendOver(out.pixels[i], pal.copper[static_cast<size_t>(klass)]);
            ++out.copperCells;
        }
    }

    // --- Pass E: Copper denied the bus. ---------------------------------------------------------
    // The Copper asked for a slot and did not get it. That is the cause of a jittering colour
    // split, and it is invisible everywhere else in the debugger.
    if (opt.showCopper && opt.showCopperWanted && haveDma) {
        for (int line = 0; line < h; ++line) {
            for (int cc = 0; cc < w; ++cc) {
                const auto* r = slotAt(line, cc);
                if (!r || !(r->evt & kEvtCopperWanted))
                    continue;
                const size_t i = idx(line, cc);
                out.cells[i].copperWanted = true;
                out.pixels[i] = BlendOver(out.pixels[i], pal.copperWanted);
            }
        }
    }

    // --- Pass F: per-line DDF / DIW marks. -------------------------------------------------------
    // DebuggerDisplayView already draws DIW and DDF as one box from the current register values,
    // i.e. the end-of-frame state flattened over the whole screen. These are where they actually
    // fired on each line, so a copper-driven change shows as a stepped edge instead.
    if (opt.showGeometryEvents && haveDma) {
        for (int line = 0; line < h; ++line) {
            for (int cc = 0; cc < w; ++cc) {
                const auto* r = slotAt(line, cc);
                if (!r || r->evt == 0)
                    continue;
                const size_t i = idx(line, cc);
                if (r->evt & (kEvtDdfStrt | kEvtDdfStop | kEvtDdfStop2))
                    out.pixels[i] = BlendOver(out.pixels[i], pal.ddfMark);
                if (r->evt & (kEvtHdiws | kEvtHdiwe))
                    out.pixels[i] = BlendOver(out.pixels[i], pal.diwMark);
            }
        }
    }

    // --- Pass G: dim whatever the beam had not reached at a mid-frame breakpoint. ---------------
    if (opt.breakLRef != UINT32_MAX) {
        constexpr uint32_t kCCKPerLine = constants::beam::CCKPerLine;
        for (int line = 0; line < h; ++line) {
            for (int cc = 0; cc < w; ++cc) {
                const uint32_t l = static_cast<uint32_t>(line) * kCCKPerLine + cc;
                if (l < opt.breakLRef)
                    continue;
                const size_t i = idx(line, cc);
                out.pixels[i] = WithAlpha(out.pixels[i], AlphaOf(out.pixels[i]) / 4u);
            }
        }
    }

    // --- Pass H: the wrap columns. ---------------------------------------------------------
    // A displayed scanline is longer than an Agnus line (`denisehtotal = maxhpos + 7`, custom.c):
    // its rightmost clocks are drawn while Agnus is already on the next line. Those columns exist
    // in the raster only because the picture reaches into them, and the slots that fill them are
    // the next line's first ones - from the row's own line length, 227 on PAL, which is where
    // Denise's counter carries on into the next Agnus line. Copying at the end rather than
    // sampling in each pass means every layer above -- blanking, channels, wait spans, copper,
    // the break dim -- carries over exactly as it was drawn, so the two views of the same clock
    // can never disagree. The last row's next line is the next frame's first: nothing to show.
    for (int line = 0; line < h; ++line) {
        const int rowCC = out.LineCCAt(line);
        if (rowCC >= w)
            continue;
        const int extra = w - rowCC;
        const size_t dst = idx(line, rowCC);
        if (line + 1 < h) {
            const size_t src = idx(line + 1, 0);
            std::copy_n(&out.pixels[src], extra, &out.pixels[dst]);
            std::copy_n(&out.cells[src], extra, &out.cells[dst]);
        } else {
            std::fill_n(&out.pixels[dst], extra, 0u);
            std::fill_n(&out.cells[dst], extra, Cell{});
        }
    }

    // --- Pass I: display-window and DDF events timed past the end of the line. -----------------
    // PuAE finishes a displayed line during the NEXT Agnus line (hsync_handlerh runs after vpos
    // has advanced), so an event on the tail of line V - a display window closing at Denise clock
    // 224.5, beam clock 228 - is recorded on row V + 1 at its unwrapped hpos. Its column is right
    // as it stands; only the row is one late. Measured: DIWSTOP moved past the end of the OCS
    // Denise counter on one line left the HDIWE missing from the FOLLOWING row, not its own.
    if (opt.showGeometryEvents && haveDma) {
        for (int line = 0; line + 1 < h && line + 1 < in.vposCount; ++line) {
            const auto* next = &in.records[static_cast<size_t>(line + 1) * in.hposCount];
            const int limit = std::min(w, in.hposCount);
            for (int cc = out.LineCCAt(line); cc < limit; ++cc) {
                const uint32_t evt = next[cc].evt;
                if (evt == 0)
                    continue;
                const size_t i = idx(line, cc);
                if (evt & (kEvtDdfStrt | kEvtDdfStop | kEvtDdfStop2))
                    out.pixels[i] = BlendOver(out.pixels[i], pal.ddfMark);
                if (evt & (kEvtHdiws | kEvtHdiwe))
                    out.pixels[i] = BlendOver(out.pixels[i], pal.diwMark);
            }
        }
    }
}

} // namespace adm::core::copraster
