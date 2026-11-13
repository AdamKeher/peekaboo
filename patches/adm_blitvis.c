
/* ==========================================================================
 * adm_blitvis -- blitter destination provenance visualisation
 *
 * Adapted from the ENGINE9000 68k/z80 retro debugger by alpine9000:
 *     https://github.com/alpine9000/engine9000-public
 *     ami9000/sources/src/{blitter,custom,drawing}.c   (commit 0d8db93)
 *     e9k-debugger/amiga_blittervis.c                  (colour hash)
 * Copyright (C) alpine9000.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.  See COPYING.
 *
 * Adapted for amiga_disk_manager (also GPL-2).  This file is injected into
 * libretro/libretro-core.c by scripts/build_puae.{ps1,sh}; the hooks that
 * call into it are patched into blitter.c, custom.c and drawing.c by the
 * same scripts.
 *
 * ---------------------------------------------------------------------------
 * THE TECHNIQUE
 *
 * The obvious way to draw "where did this blit land on screen" is to take
 * bltdpt, undo descending mode, and hunt for a bitplane buffer whose base,
 * stride and height bracket that address.  That is what BlitScreenMapper.h
 * does, and it has to guess the target plane, guess the buffer base, and it
 * still knows nothing about BPLxMOD, fetch mode or x-scroll delay.  Each guess
 * is a chance to put the box in the wrong place.
 *
 * This sidesteps the geometry problem instead of solving it, in three stages:
 *
 *   1. WRITE TIME (blitter.c).  Every word the blitter writes to chip RAM
 *      stamps a slot in a flat table indexed purely by address:
 *      (addr & chipmem_bank.mask) >> 1.  There is no row, column or stride
 *      concept at this layer at all -- so bltdmod, dual playfield and
 *      offscreen buffers are simply not this stage's problem.
 *
 *   2. FETCH TIME (custom.c).  Rather than reimplementing the display fetch,
 *      we ride on it.  Every time Agnus genuinely fetches a bitplane word we
 *      ask "did the blitter last write this word?".  If so we tag the pixel
 *      coordinates the fetch itself just computed.  Those are already correct
 *      for BPLxMOD, fetch mode, x-scroll delay and plane number, because they
 *      are the emulator's own display logic -- this is a thin tag riding
 *      piggyback on it, not a second implementation.
 *
 *   3. PROJECTION (drawing.c).  Stage 2 records into an oversized "virtual
 *      scanline" with a margin either side, so fetches slightly outside the
 *      nominal visible window are still captured.  During the normal
 *      line-to-screen decode that is projected onto final native output
 *      coordinates, post-crop and post-scale, whatever resolution the core is
 *      emitting.  The result is emitted as (x, y, xEnd, blitId) spans.
 *
 * CONSEQUENCE, which is a property of the technique and not a defect: a blit
 * whose destination is never pointed at by an active BPLxPT produces no spans
 * at all, because nothing ever fetches those words.  Read this as "memory
 * provenance of whatever is currently on screen", not as a general offscreen
 * buffer viewer.  The geometry-based overlay is retained alongside it for
 * exactly that case.
 *
 * DELIBERATE DEVIATIONS from the upstream implementation:
 *
 *   - Upstream keeps a per-word epoch plus a dirty list so the provenance map
 *     can be cleared per frame, and a separate decay counter for overlay
 *     persistence.  We instead let a word keep its stamp until another blit
 *     overwrites it, and use the "collected" flag alone to ensure one write is
 *     tagged at most once.  Visual persistence is done in the UI, which is
 *     both simpler here and more flexible there.
 *
 *   - Upstream carries its own blitter_debug_blit_meta table describing each
 *     blit.  We already record richer per-blit data in adm_record_blit(), so
 *     the blit id is allocated there and the two join on that id.
 *
 *   - Upstream forces the fast long-fetch path onto its slow per-word branch
 *     so it can tag inside it.  We instead tag in a separate pass over the
 *     same addresses, which leaves the real fetch path untouched.
 * ========================================================================== */

/* The source grid is addressed from two different coordinate spaces, and the two
   biases below are what reconcile them -- this is the least obvious thing in the
   file, so spelling it out:

     - The FETCH side (adm_blitvis_fetch / _long_fetch) counts pixels from the
       start of the line's bitplane DMA, so its coordinates start near 0 and can
       go slightly negative once the x-scroll delay is subtracted.  It writes at
       index (pixel + ADM_BLITVIS_SRC_BIAS).

     - The PROJECT side is handed drawing.c's src_pixel, which is initialised to
       MAX_PIXELS_PER_LINE + res_shift_from_window(leftborderhidden) (drawing.c,
       pfield_init_linetoscr).  So its coordinates start near MAX_PIXELS_PER_LINE.
       It reads at index (src_pixel + ADM_BLITVIS_SRC_INDEX_OFF).

   BIAS - INDEX_OFF == MAX_PIXELS_PER_LINE exactly cancels that offset, and the
   ±ADM_BLITVIS_SRC_MARGIN either side leaves room for fetches that fall outside
   the nominal visible window.  Change one of these and you must change all three. */
#define ADM_BLITVIS_LINES          ((MAXVPOS + MAXVPOS_WRAPLINES) * 2)
#define ADM_BLITVIS_SRC_MARGIN     256
#define ADM_BLITVIS_SRC_PIXELS     ((MAX_PIXELS_PER_LINE * 2) + 1 + (ADM_BLITVIS_SRC_MARGIN * 2))
#define ADM_BLITVIS_SRC_BIAS       (MAX_PIXELS_PER_LINE + ADM_BLITVIS_SRC_MARGIN)
#define ADM_BLITVIS_SRC_INDEX_OFF  (ADM_BLITVIS_SRC_BIAS - MAX_PIXELS_PER_LINE)
#define ADM_BLITVIS_EDGE_NATIVE_PX 32
#define ADM_BLITVIS_EDGE_SOURCE_PX 64
#define ADM_BLITVIS_MAX_SPANS      65536

/* DLL wire format -- must match AmigaEmulator::BlitVisSpan byte for byte. */
typedef struct adm_blitvis_span
{
    uint16_t x;      /* first native pixel of the span (inclusive) */
    uint16_t y;      /* native scanline */
    uint16_t xEnd;   /* last native pixel of the span (inclusive) */
    uint16_t pad;    /* explicit padding -- keep the struct at 12 bytes */
    uint32_t blitId; /* joins onto adm_blit_event_t.blitId */
} adm_blitvis_span_t;

/* Non-static: the hooks in blitter.c / custom.c / drawing.c test this directly
   so that the cost when the overlay is off is one global load and a branch,
   not a call into here. */
int              adm_blitvis_enabled        = 0;
static uint32_t *adm_blitvis_src_ids        = NULL; /* [LINES][SRC_PIXELS] */
static uint32_t *adm_blitvis_src_epoch      = NULL; /* [LINES] */
static uint32_t *adm_blitvis_native_ids     = NULL; /* [LINES][MAX_PIXELS_PER_LINE] */
static int32_t  *adm_blitvis_native_minx    = NULL; /* [LINES] */
static int32_t  *adm_blitvis_native_maxx    = NULL; /* [LINES] */
static uint32_t *adm_blitvis_word_blit      = NULL; /* [word_count] */
static uint8_t  *adm_blitvis_word_collected = NULL; /* [word_count] */
static uint32_t  adm_blitvis_word_count     = 0;
static uint32_t  adm_blitvis_src_epoch_ctr  = 1;

static adm_blitvis_span_t *adm_blitvis_spans = NULL;
static uint32_t adm_blitvis_span_count = 0;

static int adm_blitvis_pixel_cursor[MAX_PLANES];
static int adm_blitvis_src_line_ctx    = -1;
static int adm_blitvis_native_line_ctx = -1;

/* A line drawn as LINE_AS_PREVIOUS re-displays an earlier line's decoded data,
   so its tags live under that earlier line's index, not its own.  Track what
   each drawn line actually resolved to. */
#define ADM_BLITVIS_LINESTATE (ADM_BLITVIS_LINES + 1)
static int *adm_blitvis_resolved_src        = NULL; /* [ADM_BLITVIS_LINESTATE] */
static int  adm_blitvis_last_resolved_src   = -1;
static int  adm_blitvis_pending_as_previous = 0;

/* Diagnostics -- surfaced by adm_get_blitter_vis_stats so the UI can tell
   "no blits happened" apart from "the DLL predates this patch". */
static uint32_t adm_blitvis_writes_frame = 0, adm_blitvis_writes_last = 0;
static uint32_t adm_blitvis_tags_frame   = 0, adm_blitvis_tags_last   = 0;
static uint32_t adm_blitvis_marks_frame  = 0, adm_blitvis_marks_last  = 0;

/* Blit id allocation.  Runs whether or not the visualiser is enabled, so
   adm_blit_event_t.blitId is always meaningful; 0 is reserved for "no blit". */
uint32_t adm_blitvis_current_blit_id = 0;
static uint32_t adm_blitvis_next_blit_id = 1;

uint32_t adm_blitvis_begin_blit(void)
{
    adm_blitvis_current_blit_id = adm_blitvis_next_blit_id++;
    if (adm_blitvis_next_blit_id == 0)
        adm_blitvis_next_blit_id = 1;
    return adm_blitvis_current_blit_id;
}

/* ---- storage ----------------------------------------------------------- */

static void adm_blitvis_free_storage(void)
{
    free(adm_blitvis_src_ids);        adm_blitvis_src_ids = NULL;
    free(adm_blitvis_src_epoch);      adm_blitvis_src_epoch = NULL;
    free(adm_blitvis_native_ids);     adm_blitvis_native_ids = NULL;
    free(adm_blitvis_native_minx);    adm_blitvis_native_minx = NULL;
    free(adm_blitvis_native_maxx);    adm_blitvis_native_maxx = NULL;
    free(adm_blitvis_word_blit);      adm_blitvis_word_blit = NULL;
    free(adm_blitvis_word_collected); adm_blitvis_word_collected = NULL;
    free(adm_blitvis_spans);          adm_blitvis_spans = NULL;
    free(adm_blitvis_resolved_src);   adm_blitvis_resolved_src = NULL;
    adm_blitvis_word_count = 0;
    adm_blitvis_span_count = 0;
    adm_blitvis_src_epoch_ctr = 1;
}

/* Roughly 50 MB at 2 MB chip, and only while the overlay is switched on --
   the tables are freed again on disable. */
static int adm_blitvis_ensure_storage(void)
{
    uint32_t words = (uint32_t)((chipmem_bank.mask + 1u) >> 1);

    if (adm_blitvis_src_ids && adm_blitvis_word_count == words)
        return 1;

    adm_blitvis_free_storage();
    if (words == 0)
        return 0;

    adm_blitvis_src_ids        = (uint32_t *)calloc((size_t)ADM_BLITVIS_LINES * ADM_BLITVIS_SRC_PIXELS, sizeof(uint32_t));
    adm_blitvis_src_epoch      = (uint32_t *)calloc((size_t)ADM_BLITVIS_LINES, sizeof(uint32_t));
    adm_blitvis_native_ids     = (uint32_t *)calloc((size_t)ADM_BLITVIS_LINES * MAX_PIXELS_PER_LINE, sizeof(uint32_t));
    adm_blitvis_native_minx    = (int32_t *)malloc((size_t)ADM_BLITVIS_LINES * sizeof(int32_t));
    adm_blitvis_native_maxx    = (int32_t *)malloc((size_t)ADM_BLITVIS_LINES * sizeof(int32_t));
    adm_blitvis_word_blit      = (uint32_t *)calloc(words, sizeof(uint32_t));
    adm_blitvis_word_collected = (uint8_t *)calloc(words, sizeof(uint8_t));
    adm_blitvis_spans          = (adm_blitvis_span_t *)calloc(ADM_BLITVIS_MAX_SPANS, sizeof(adm_blitvis_span_t));
    adm_blitvis_resolved_src   = (int *)malloc((size_t)ADM_BLITVIS_LINESTATE * sizeof(int));

    if (!adm_blitvis_resolved_src || !adm_blitvis_src_ids || !adm_blitvis_src_epoch || !adm_blitvis_native_ids ||
        !adm_blitvis_native_minx || !adm_blitvis_native_maxx || !adm_blitvis_word_blit ||
        !adm_blitvis_word_collected || !adm_blitvis_spans) {
        adm_blitvis_free_storage();
        return 0;
    }

    for (int y = 0; y < ADM_BLITVIS_LINES; y++) {
        adm_blitvis_native_minx[y] = -1;
        adm_blitvis_native_maxx[y] = -1;
    }
    for (int y = 0; y < ADM_BLITVIS_LINESTATE; y++)
        adm_blitvis_resolved_src[y] = -1;
    adm_blitvis_word_count = words;
    adm_blitvis_src_epoch_ctr = 1;
    return 1;
}

static int adm_blitvis_native_width(void)
{
    if (retrow_crop > 0 && retrow_crop <= MAX_PIXELS_PER_LINE)
        return (int)retrow_crop;
    return MAX_PIXELS_PER_LINE;
}

static int adm_blitvis_native_height(void)
{
    if (retroh_crop > 0 && retroh_crop <= ADM_BLITVIS_LINES)
        return (int)retroh_crop;
    return ADM_BLITVIS_LINES;
}

/* ---- stage 1: write-time provenance ------------------------------------ */

void adm_blitvis_record_write(uaecptr addr)
{
    uint32_t index;
    if (!adm_blitvis_enabled || !adm_blitvis_word_blit)
        return;
    index = (uint32_t)((addr & chipmem_bank.mask) >> 1);
    if (index >= adm_blitvis_word_count)
        return;
    adm_blitvis_word_blit[index] = adm_blitvis_current_blit_id;
    adm_blitvis_word_collected[index] = 0;
    adm_blitvis_writes_frame++;
}

/* Returns the blit that last wrote this word, and consumes the tag so a word
   re-fetched later in the same frame is not counted twice.  Re-armed only by
   the next blitter write to that address. */
static uint32_t adm_blitvis_tag_fetch(uaecptr addr)
{
    uint32_t index, id;
    if (!adm_blitvis_word_blit)
        return 0;
    index = (uint32_t)((addr & chipmem_bank.mask) >> 1);
    if (index >= adm_blitvis_word_count)
        return 0;
    id = adm_blitvis_word_blit[index];
    if (!id || adm_blitvis_word_collected[index])
        return 0;
    adm_blitvis_word_collected[index] = 1;
    adm_blitvis_tags_frame++;
    return id;
}

/* ---- stage 2: fetch-time tagging --------------------------------------- */

void adm_blitvis_reset_line_cursors(void)
{
    int p;
    for (p = 0; p < MAX_PLANES; p++)
        adm_blitvis_pixel_cursor[p] = 0;
}

static int adm_blitvis_take_pixel_start(int plane, int pixel_count)
{
    int start;
    if (plane < 0 || plane >= MAX_PLANES)
        return -1;
    start = adm_blitvis_pixel_cursor[plane];
    adm_blitvis_pixel_cursor[plane] += pixel_count;
    return start;
}

void adm_blitvis_mark_source_range(int lineno, int pixel_start, int pixel_count, uint32_t blit_id)
{
    int start, end, x;
    uint32_t *row;

    if (!adm_blitvis_src_ids || !blit_id || pixel_count <= 0)
        return;
    if (lineno < 0 || lineno >= ADM_BLITVIS_LINES)
        return;

    start = pixel_start + ADM_BLITVIS_SRC_BIAS;
    end   = pixel_start + pixel_count + ADM_BLITVIS_SRC_BIAS;
    if (end <= 0 || start >= ADM_BLITVIS_SRC_PIXELS)
        return;
    if (start < 0)
        start = 0;
    if (end > ADM_BLITVIS_SRC_PIXELS)
        end = ADM_BLITVIS_SRC_PIXELS;

    row = adm_blitvis_src_ids + (size_t)lineno * ADM_BLITVIS_SRC_PIXELS;
    if (adm_blitvis_src_epoch[lineno] != adm_blitvis_src_epoch_ctr) {
        memset(row, 0, (size_t)ADM_BLITVIS_SRC_PIXELS * sizeof(uint32_t));
        adm_blitvis_src_epoch[lineno] = adm_blitvis_src_epoch_ctr;
    }
    adm_blitvis_marks_frame++;
    for (x = start; x < end; x++)
        row[x] = blit_id;
}

/* Phase correction between "which word Agnus fetched" and "which pixel it
   ends up as".  pipeline_advance accounts for the display pipeline already
   running (fetched words map one fetch-unit ahead), display_delay for the
   BPLCON1 x-scroll.  Empirical, from upstream -- if spans come out shifted by
   a constant number of pixels, this is where to look. */
static int adm_blitvis_mark_pixel_start(int pixel_start, int pipeline_advance, int display_delay)
{
    if (pixel_start < 0)
        return -1;
    return pixel_start + pipeline_advance + display_delay;
}

/* Per-word fetch: custom.c fetch().  words = 1 << fm chip words, each of
   which becomes 16 source pixels. */
void adm_blitvis_fetch(int lineno, uaecptr addr, int plane, int fm, int delay, int pipeline_advance)
{
    int words, i, base;

    if (!adm_blitvis_enabled || !adm_blitvis_word_blit)
        return;
    if (plane < 0 || plane >= MAX_PLANES)
        return;

    words = 1 << fm;
    /* Mirror the alignment the real fetch applies: fetch32 uses p & ~3,
       fetch64 uses p & ~7. */
    if (fm > 0)
        addr &= ~(uaecptr)((2u << fm) - 1u);

    base = adm_blitvis_take_pixel_start(plane, 16 << fm);
    if (base < 0)
        return;

    for (i = 0; i < words; i++) {
        uint32_t id = adm_blitvis_tag_fetch(addr + (uaecptr)(i * 2));
        if (id) {
            int px = adm_blitvis_mark_pixel_start(base + i * 16, pipeline_advance, delay);
            if (px >= 0)
                adm_blitvis_mark_source_range(lineno, px, 16, id);
        }
    }
}

/* Batch fetch: custom.c long_fetch_16/32/64.  nwords counts 16-bit words in
   all three, so the pixel arithmetic is uniform.  Runs of consecutive words
   belonging to the same blit are coalesced into a single mark. */
void adm_blitvis_long_fetch(int lineno, uaecptr addr, int plane, int nwords, int delay)
{
    int i;
    int run_start = -1, run_count = 0;
    uint32_t run_id = 0;

    if (!adm_blitvis_enabled || !adm_blitvis_word_blit)
        return;
    if (plane < 0 || plane >= MAX_PLANES || nwords <= 0)
        return;

    for (i = 0; i < nwords; i++) {
        int base = adm_blitvis_take_pixel_start(plane, 16);
        uint32_t id;
        int px;

        if (base < 0)
            break;
        id = adm_blitvis_tag_fetch(addr + (uaecptr)(i * 2));
        px = adm_blitvis_mark_pixel_start(base, 16, delay);

        if (id && px >= 0 && run_id == id && px == run_start + run_count) {
            run_count += 16;
            continue;
        }
        if (run_count > 0)
            adm_blitvis_mark_source_range(lineno, run_start, run_count, run_id);
        if (id && px >= 0) {
            run_start = px;
            run_count = 16;
            run_id = id;
        } else {
            run_start = -1;
            run_count = 0;
            run_id = 0;
        }
    }
    if (run_count > 0)
        adm_blitvis_mark_source_range(lineno, run_start, run_count, run_id);
}

/* ---- stage 3: source -> native projection ------------------------------ */

void adm_blitvis_set_line_context(int source_line, int native_line)
{
    adm_blitvis_src_line_ctx    = -1;
    adm_blitvis_native_line_ctx = -1;
    if (source_line >= 0 && source_line < ADM_BLITVIS_LINES)
        adm_blitvis_src_line_ctx = source_line;
    if (native_line >= 0 && native_line < ADM_BLITVIS_LINES)
        adm_blitvis_native_line_ctx = native_line;
}

/* drawing.c: the LINE_AS_PREVIOUS arm of pfield_draw_line's switch. */
void adm_blitvis_note_as_previous(void)
{
    adm_blitvis_pending_as_previous = 1;
}

/* drawing.c: called once per drawn line, after that switch has run.  Resolves
   which source line actually holds this line's tags and installs the context
   the projection pass reads. */
void adm_blitvis_begin_line(int lineno, int native_line)
{
    int src = lineno;

    if (!adm_blitvis_enabled) {
        adm_blitvis_pending_as_previous = 0;
        return;
    }

    if (adm_blitvis_pending_as_previous) {
        adm_blitvis_pending_as_previous = 0;
        if (adm_blitvis_last_resolved_src >= 0) {
            src = adm_blitvis_last_resolved_src;
        } else {
            src = lineno - 1;
            if (lineno > 0 && adm_blitvis_resolved_src && lineno - 1 < ADM_BLITVIS_LINESTATE) {
                int resolved = adm_blitvis_resolved_src[lineno - 1];
                if (resolved >= 0)
                    src = resolved;
            }
        }
    }

    if (src < 0 || src >= ADM_BLITVIS_LINES)
        src = -1;
    if (adm_blitvis_resolved_src && lineno >= 0 && lineno < ADM_BLITVIS_LINESTATE)
        adm_blitvis_resolved_src[lineno] = src;
    if (src >= 0)
        adm_blitvis_last_resolved_src = src;

    adm_blitvis_set_line_context(src, native_line);
}

static void adm_blitvis_mark_native_range(int pixel_start, int pixel_count, uint32_t blit_id)
{
    int native_y, native_w, native_h, start, end, x;
    uint32_t *row;

    if (!adm_blitvis_native_ids || !blit_id || pixel_count <= 0)
        return;
    if (adm_blitvis_native_line_ctx < 0)
        return;

    native_y = adm_blitvis_native_line_ctx - (int)retroy_crop;
    native_h = adm_blitvis_native_height();
    if (native_y < 0 || native_y >= native_h)
        return;

    start = pixel_start - (int)retrox_crop;
    end   = start + pixel_count;
    native_w = adm_blitvis_native_width();
    if (end <= 0 || start >= native_w)
        return;
    if (start < 0)
        start = 0;
    if (end > native_w)
        end = native_w;

    row = adm_blitvis_native_ids + (size_t)native_y * MAX_PIXELS_PER_LINE;
    for (x = start; x < end; x++)
        row[x] = blit_id;

    if (adm_blitvis_native_minx[native_y] < 0 || start < adm_blitvis_native_minx[native_y])
        adm_blitvis_native_minx[native_y] = start;
    if (end - 1 > adm_blitvis_native_maxx[native_y])
        adm_blitvis_native_maxx[native_y] = end - 1;
}

static uint32_t adm_blitvis_find_id(const uint32_t *row, int range_start, int range_end)
{
    int x;
    if (range_start < 0)
        range_start = 0;
    if (range_end > ADM_BLITVIS_SRC_PIXELS)
        range_end = ADM_BLITVIS_SRC_PIXELS;
    for (x = range_start; x < range_end; x++) {
        if (row[x])
            return row[x];
    }
    return 0;
}

static uint32_t adm_blitvis_find_id_from(const uint32_t *row, int range_start, int range_end, int *cursor)
{
    int x;
    if (range_start < 0)
        range_start = 0;
    if (range_end > ADM_BLITVIS_SRC_PIXELS)
        range_end = ADM_BLITVIS_SRC_PIXELS;
    if (range_start >= range_end) {
        if (*cursor < range_start)
            *cursor = range_start;
        return 0;
    }
    if (*cursor < range_start)
        *cursor = range_start;
    else if (*cursor > range_end)
        *cursor = range_end;
    if (*cursor >= range_end)
        return 0;
    for (x = *cursor; x < range_end; x++) {
        if (row[x]) {
            *cursor = x;
            return row[x];
        }
    }
    *cursor = range_end;
    return 0;
}

/* Called from drawing.c once per linetoscr segment.  [source_start,source_end)
   is the decoded-pixel range that produced native pixels [native_start,
   native_end); x_adjust is linetoscr_x_adjust_pixels. */
void adm_blitvis_project(int source_start, int source_end, int native_start, int native_end, int x_adjust)
{
    int source_width, native_width, source_cursor, native_x;
    int run_start = 0, run_count = 0;
    uint32_t run_id = 0;
    const uint32_t *row;

    if (!adm_blitvis_enabled || !adm_blitvis_src_ids)
        return;
    if (adm_blitvis_marks_frame == 0)
        return;
    if (adm_blitvis_src_line_ctx < 0)
        return;
    if (source_end <= source_start || native_end <= native_start)
        return;
    if (adm_blitvis_src_epoch[adm_blitvis_src_line_ctx] != adm_blitvis_src_epoch_ctr)
        return;

    source_width = source_end - source_start;
    native_width = native_end - native_start;
    row = adm_blitvis_src_ids + (size_t)adm_blitvis_src_line_ctx * ADM_BLITVIS_SRC_PIXELS;
    source_cursor = source_start + ADM_BLITVIS_SRC_INDEX_OFF;

    for (native_x = native_start; native_x < native_end; native_x++) {
        int rel_start = native_x - native_start;
        int rel_end   = rel_start + 1;
        int src_range_start = source_start + (int)(((int64_t)rel_start * source_width) / native_width);
        int src_range_end   = source_start + (int)((((int64_t)rel_end * source_width) + native_width - 1) / native_width);
        int store_start, store_end, from_left, from_right;
        uint32_t blit_id;

        if (src_range_end <= src_range_start)
            src_range_end = src_range_start + 1;
        store_start = src_range_start + ADM_BLITVIS_SRC_INDEX_OFF;
        store_end   = src_range_end + ADM_BLITVIS_SRC_INDEX_OFF;
        from_left   = native_x - native_start;
        from_right  = (native_end - 1) - native_x;

        blit_id = adm_blitvis_find_id_from(row, store_start, store_end, &source_cursor);

        /* Near either edge of the segment, widen the search a little: the
           fetch that produced an edge pixel can sit just outside the range the
           ratio above computes. */
        if (blit_id == 0) {
            int left_start = store_start, left_end = store_start;
            int right_start = store_end, right_end = store_end;
            if (from_left < ADM_BLITVIS_EDGE_NATIVE_PX)
                left_start -= ADM_BLITVIS_EDGE_SOURCE_PX;
            if (from_right < ADM_BLITVIS_EDGE_NATIVE_PX)
                right_end += ADM_BLITVIS_EDGE_SOURCE_PX;
            blit_id = adm_blitvis_find_id(row, right_start, right_end);
            if (blit_id == 0)
                blit_id = adm_blitvis_find_id(row, left_start, left_end);
        }

        if (blit_id != 0 && run_count > 0 && run_id == blit_id) {
            run_count++;
            continue;
        }
        if (run_count > 0)
            adm_blitvis_mark_native_range(run_start - x_adjust, run_count, run_id);
        if (blit_id != 0) {
            run_id = blit_id;
            run_start = native_x;
            run_count = 1;
        } else {
            run_id = 0;
            run_start = 0;
            run_count = 0;
        }
    }
    if (run_count > 0)
        adm_blitvis_mark_native_range(run_start - x_adjust, run_count, run_id);
}

/* ---- frame lifecycle --------------------------------------------------- */

/* Collapse the native pixel grid into spans and clear it for the next frame. */
static void adm_blitvis_snapshot_frame(void)
{
    int native_h = adm_blitvis_native_height();
    int y;

    adm_blitvis_span_count = 0;
    if (!adm_blitvis_native_ids)
        return;

    for (y = 0; y < native_h; y++) {
        int start = adm_blitvis_native_minx[y];
        int end   = adm_blitvis_native_maxx[y];
        uint32_t *row;
        int x;

        if (start < 0 || end < start)
            continue;
        row = adm_blitvis_native_ids + (size_t)y * MAX_PIXELS_PER_LINE;

        for (x = start; x <= end; x++) {
            uint32_t blit_id = row[x];
            int span_end;
            adm_blitvis_span_t *span;

            if (!blit_id)
                continue;
            span_end = x;
            while (span_end + 1 <= end && row[span_end + 1] == blit_id)
                span_end++;

            if (adm_blitvis_span_count < ADM_BLITVIS_MAX_SPANS) {
                span = &adm_blitvis_spans[adm_blitvis_span_count++];
                span->x      = (uint16_t)x;
                span->y      = (uint16_t)y;
                span->xEnd   = (uint16_t)span_end;
                span->pad    = 0;
                span->blitId = blit_id;
            }
            x = span_end;
        }

        /* Clear only what we dirtied -- a full clear of the native grid every
           frame would be 15 MB of memset. */
        memset(row + start, 0, (size_t)(end - start + 1) * sizeof(uint32_t));
        adm_blitvis_native_minx[y] = -1;
        adm_blitvis_native_maxx[y] = -1;
    }
}

/* Called at vsync, from custom.c init_hardware_for_drawing_frame(). */
void adm_blitvis_frame_begin(void)
{
    if (!adm_blitvis_enabled)
        return;

    adm_blitvis_snapshot_frame();

    adm_blitvis_writes_last = adm_blitvis_writes_frame;
    adm_blitvis_tags_last   = adm_blitvis_tags_frame;
    adm_blitvis_marks_last  = adm_blitvis_marks_frame;
    adm_blitvis_writes_frame = 0;
    adm_blitvis_tags_frame   = 0;
    adm_blitvis_marks_frame  = 0;

    /* Bump the source epoch: every source line is lazily zeroed on first mark. */
    adm_blitvis_src_epoch_ctr++;
    if (adm_blitvis_src_epoch_ctr == 0) {
        memset(adm_blitvis_src_epoch, 0, (size_t)ADM_BLITVIS_LINES * sizeof(uint32_t));
        adm_blitvis_src_epoch_ctr = 1;
    }
    adm_blitvis_src_line_ctx    = -1;
    adm_blitvis_native_line_ctx = -1;
    adm_blitvis_last_resolved_src = -1;
    adm_blitvis_pending_as_previous = 0;
    if (adm_blitvis_resolved_src) {
        int i;
        for (i = 0; i < ADM_BLITVIS_LINESTATE; i++)
            adm_blitvis_resolved_src[i] = -1;
    }
    adm_blitvis_reset_line_cursors();
}

/* The provenance map is transient per-frame state and is not part of a
   savestate.  Drop it on restore rather than letting stale stamps describe
   memory that has been wholesale replaced. */
void adm_blitvis_reset(void)
{
    if (adm_blitvis_word_blit) {
        memset(adm_blitvis_word_blit, 0, (size_t)adm_blitvis_word_count * sizeof(uint32_t));
        memset(adm_blitvis_word_collected, 0, (size_t)adm_blitvis_word_count);
    }
    adm_blitvis_span_count = 0;
}

/* ---- exports ----------------------------------------------------------- */

RETRO_API void adm_set_blitter_vis_enabled(int enabled)
{
    if (enabled) {
        if (!adm_blitvis_ensure_storage()) {
            adm_blitvis_enabled = 0;
            return;
        }
        adm_blitvis_reset();
        adm_blitvis_reset_line_cursors();
        adm_blitvis_enabled = 1;
    } else {
        adm_blitvis_enabled = 0;
        adm_blitvis_free_storage();
    }
}

RETRO_API int adm_get_blitter_vis_spans(void *buf, int max_count)
{
    int count = (int)adm_blitvis_span_count;
    if (!buf || max_count <= 0)
        return 0;
    if (count > max_count)
        count = max_count;
    memcpy(buf, adm_blitvis_spans, (size_t)count * sizeof(adm_blitvis_span_t));
    return count;
}

/* Lets the UI distinguish "no blits this frame" from "this DLL predates the
   patch" -- the same diagnostic the Sprites window uses for sprpt events. */
RETRO_API void adm_get_blitter_vis_stats(int *enabled, int *writes, int *tags, int *spans)
{
    if (enabled) *enabled = adm_blitvis_enabled;
    if (writes)  *writes  = (int)adm_blitvis_writes_last;
    if (tags)    *tags    = (int)adm_blitvis_tags_last;
    if (spans)   *spans   = (int)adm_blitvis_span_count;
}
