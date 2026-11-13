// Tests for the span-to-pixel half of the blit provenance overlay
// (src/core/BlitVisRaster.h).
//
// The core hands us spans that are already in native output coordinates (see
// patches/adm_blitvis.c and AdmBlitVisTest.cpp), so there is no coordinate mapping to verify
// here. What is left is the painting itself: clipping, per-blit colouring, the isolation
// dimming used when a blit is selected, and the decay that keeps a one-shot blit visible for
// more than a single frame.

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "../src/core/BlitVisRaster.h"

namespace blitvis = adm::core::blitvis;
using Span = adm::AmigaEmulator::BlitVisSpan;

namespace {

constexpr int kW = 32;
constexpr int kH = 8;

std::vector<uint32_t> MakeBuffer()
{
    return std::vector<uint32_t>(static_cast<size_t>(kW) * kH, 0u);
}

uint32_t At(const std::vector<uint32_t>& px, int x, int y)
{
    return px[static_cast<size_t>(y) * kW + static_cast<size_t>(x)];
}

uint32_t AlphaAt(const std::vector<uint32_t>& px, int x, int y) { return At(px, x, y) >> 24; }

Span MakeSpan(uint16_t x, uint16_t y, uint16_t xEnd, uint32_t blitId)
{
    Span s{};
    s.x = x;
    s.y = y;
    s.xEnd = xEnd;
    s.pad = 0;
    s.blitId = blitId;
    return s;
}

// ---- colouring ----------------------------------------------------------------------------

TEST(BlitterVisOverlayColor, IsStableForAGivenBlit)
{
    // The overlay is only readable if a blit keeps one colour while it is on screen.
    EXPECT_EQ(blitvis::ColorForBlitId(42), blitvis::ColorForBlitId(42));
    EXPECT_EQ(blitvis::ColorForBlitId(99999), blitvis::ColorForBlitId(99999));
}

TEST(BlitterVisOverlayColor, ZeroIsFullyTransparent)
{
    // 0 means "no blit" — it must never paint.
    EXPECT_EQ(blitvis::AlphaOf(blitvis::ColorForBlitId(0)), 0u);
}

TEST(BlitterVisOverlayColor, NonZeroIdsAreOpaque)
{
    for (uint32_t id = 1; id < 50; ++id)
        EXPECT_EQ(blitvis::AlphaOf(blitvis::ColorForBlitId(id)), 255u) << "blit id " << id;
}

TEST(BlitterVisOverlayColor, ConsecutiveIdsAreDistinguishable)
{
    // Blits are numbered sequentially, so neighbouring ids are exactly the ones a user has to
    // tell apart. A weak hash here would make a frame of blits look like one colour.
    int collisions = 0;
    for (uint32_t id = 1; id < 64; ++id) {
        if (blitvis::ColorForBlitId(id) == blitvis::ColorForBlitId(id + 1))
            ++collisions;
    }
    EXPECT_EQ(collisions, 0);
}

// ---- painting -----------------------------------------------------------------------------

TEST(BlitterVisOverlayPaint, FillsTheInclusiveRange)
{
    auto px = MakeBuffer();
    const Span s = MakeSpan(4, 2, 7, 1);
    blitvis::PaintSpans(px, kW, kH, &s, 1);

    EXPECT_EQ(AlphaAt(px, 3, 2), 0u) << "one before the span must stay clear";
    for (int x = 4; x <= 7; ++x)
        EXPECT_EQ(AlphaAt(px, x, 2), 255u) << "x=" << x;
    EXPECT_EQ(AlphaAt(px, 8, 2), 0u) << "xEnd is inclusive, so 8 must stay clear";
}

TEST(BlitterVisOverlayPaint, TouchesOnlyItsOwnRow)
{
    auto px = MakeBuffer();
    const Span s = MakeSpan(0, 3, 31, 1);
    blitvis::PaintSpans(px, kW, kH, &s, 1);

    for (int y = 0; y < kH; ++y) {
        const uint32_t a = AlphaAt(px, 10, y);
        if (y == 3)
            EXPECT_EQ(a, 255u);
        else
            EXPECT_EQ(a, 0u) << "row " << y << " should be untouched";
    }
}

TEST(BlitterVisOverlayPaint, ClipsToTheBufferWidth)
{
    auto px = MakeBuffer();
    // A span running past the right edge must clip, not wrap onto the next row — which is what
    // a missing bound would look like, and would be maddening to diagnose on screen.
    const Span s = MakeSpan(28, 1, 200, 7);
    blitvis::PaintSpans(px, kW, kH, &s, 1);

    EXPECT_EQ(AlphaAt(px, 31, 1), 255u);
    for (int x = 0; x < kW; ++x)
        EXPECT_EQ(AlphaAt(px, x, 2), 0u) << "row 2 must not be touched, x=" << x;
}

TEST(BlitterVisOverlayPaint, DropsRowsOutsideTheBuffer)
{
    auto px = MakeBuffer();
    const Span spans[] = {MakeSpan(0, 100, 4, 1), MakeSpan(0, 2, 4, 2)};
    blitvis::PaintSpans(px, kW, kH, spans, 2);

    EXPECT_EQ(AlphaAt(px, 0, 2), 255u) << "the in-range span should still be painted";
}

TEST(BlitterVisOverlayPaint, IgnoresZeroBlitIdAndMalformedSpans)
{
    auto px = MakeBuffer();
    const Span spans[] = {
        MakeSpan(0, 0, 4, 0),  // blitId 0 = no blit
        MakeSpan(10, 1, 5, 3), // xEnd < x
    };
    blitvis::PaintSpans(px, kW, kH, spans, 2);

    for (uint32_t p : px)
        EXPECT_EQ(p >> 24, 0u);
}

TEST(BlitterVisOverlayPaint, LaterSpansOverwriteEarlierOnes)
{
    auto px = MakeBuffer();
    // Two blits writing the same pixels in one frame: the last one to be fetched wins, which
    // matches "what is on screen came from this blit".
    const Span spans[] = {MakeSpan(0, 0, 7, 1), MakeSpan(0, 0, 7, 2)};
    blitvis::PaintSpans(px, kW, kH, spans, 2);

    EXPECT_EQ(At(px, 0, 0) & 0x00FFFFFFu, blitvis::ColorForBlitId(2) & 0x00FFFFFFu);
}

// ---- isolation ----------------------------------------------------------------------------

TEST(BlitterVisOverlayIsolation, DimsOtherBlitsRatherThanHidingThem)
{
    auto px = MakeBuffer();
    auto ids = MakeBuffer();
    const Span spans[] = {MakeSpan(0, 0, 3, 7), MakeSpan(8, 0, 11, 9)};
    blitvis::PaintSpans(px, kW, kH, spans, 2, &ids);

    std::vector<uint32_t> out;
    blitvis::ApplyIsolation(px, ids, out, /*isolate*/ 7);

    const uint32_t selected = AlphaAt(out, 0, 0);
    const uint32_t other = AlphaAt(out, 8, 0);
    EXPECT_EQ(selected, 255u) << "the selected blit stays at full strength";
    EXPECT_GT(other, 0u) << "other blits stay visible for context";
    EXPECT_LT(other, selected) << "...but dimmer than the selected one";
}

TEST(BlitterVisOverlayIsolation, LeavesTheSourceBufferUntouched)
{
    // The point of the split: isolation must not be destructive, or changing the selection
    // twice would compound the dimming and the un-isolated view could never be recovered
    // without a repaint — which is exactly what cannot happen while the emulator is paused.
    auto px = MakeBuffer();
    auto ids = MakeBuffer();
    const Span spans[] = {MakeSpan(0, 0, 3, 7), MakeSpan(8, 0, 11, 9)};
    blitvis::PaintSpans(px, kW, kH, spans, 2, &ids);

    std::vector<uint32_t> out;
    blitvis::ApplyIsolation(px, ids, out, 7);
    blitvis::ApplyIsolation(px, ids, out, 9);

    EXPECT_EQ(AlphaAt(px, 8, 0), 255u) << "the canonical buffer must not be dimmed in place";
    EXPECT_EQ(AlphaAt(out, 8, 0), 255u) << "the newly selected blit is at full strength";
    EXPECT_LT(AlphaAt(out, 0, 0), 255u) << "the previously selected one is now the dim one";
}

TEST(BlitterVisOverlayIsolation, DimmingIsACeilingNotAnAssignment)
{
    // A pixel already faded below the dim level must keep fading, not jump back up to it.
    auto px = MakeBuffer();
    auto ids = MakeBuffer();
    const Span spans[] = {MakeSpan(0, 0, 3, 7), MakeSpan(8, 0, 11, 9)};
    blitvis::PaintSpans(px, kW, kH, spans, 2, &ids);
    // The per-frame step is an integer division, so fade until it is actually under the dim
    // level rather than assuming a frame count.
    for (int i = 0; i < 32 && AlphaAt(px, 8, 0) >= blitvis::kDimmedAlpha; ++i)
        blitvis::DecayPixels(px, 10, &ids);

    const uint32_t faded = AlphaAt(px, 8, 0);
    ASSERT_LT(faded, blitvis::kDimmedAlpha);
    ASSERT_GT(faded, 0u) << "must still be visible, or the ceiling is untestable";

    std::vector<uint32_t> out;
    blitvis::ApplyIsolation(px, ids, out, 7);
    EXPECT_EQ(AlphaAt(out, 8, 0), faded);
}

TEST(BlitterVisOverlayIsolation, ZeroIsolatesNothing)
{
    auto px = MakeBuffer();
    auto ids = MakeBuffer();
    const Span spans[] = {MakeSpan(0, 0, 3, 7), MakeSpan(8, 0, 11, 9)};
    blitvis::PaintSpans(px, kW, kH, spans, 2, &ids);

    std::vector<uint32_t> out;
    blitvis::ApplyIsolation(px, ids, out, 0);

    EXPECT_EQ(AlphaAt(out, 0, 0), 255u);
    EXPECT_EQ(AlphaAt(out, 8, 0), 255u);
}

// ---- the id map (hit-testing a blit off the screen) -----------------------------------------

TEST(BlitterVisOverlayIdMap, RecordsTheOwningBlitPerPixel)
{
    auto px = MakeBuffer();
    auto ids = MakeBuffer();
    const Span spans[] = {MakeSpan(0, 0, 3, 7), MakeSpan(8, 0, 11, 9)};
    blitvis::PaintSpans(px, kW, kH, spans, 2, &ids);

    EXPECT_EQ(blitvis::BlitIdAt(ids, kW, kH, 2, 0), 7u);
    EXPECT_EQ(blitvis::BlitIdAt(ids, kW, kH, 9, 0), 9u);
    EXPECT_EQ(blitvis::BlitIdAt(ids, kW, kH, 5, 0), 0u) << "the gap belongs to no blit";
    EXPECT_EQ(blitvis::BlitIdAt(ids, kW, kH, 2, 1), 0u) << "another row belongs to no blit";
}

TEST(BlitterVisOverlayIdMap, AgreesWithThePaintedColour)
{
    // The map is what a click resolves through, so it has to name the same blit the user sees.
    auto px = MakeBuffer();
    auto ids = MakeBuffer();
    const Span spans[] = {MakeSpan(0, 0, 7, 1), MakeSpan(0, 0, 7, 2)};
    blitvis::PaintSpans(px, kW, kH, spans, 2, &ids);

    const uint32_t id = blitvis::BlitIdAt(ids, kW, kH, 0, 0);
    EXPECT_EQ(id, 2u) << "the later span wins, in the map as well as in the pixels";
    EXPECT_EQ(At(px, 0, 0) & 0x00FFFFFFu, blitvis::ColorForBlitId(id) & 0x00FFFFFFu);
}

TEST(BlitterVisOverlayIdMap, IsClearedWhenAPixelFadesOut)
{
    // A faded pixel shows nothing, so it must not still claim to belong to a blit — picking
    // there would resolve to a blit the user cannot see.
    auto px = MakeBuffer();
    auto ids = MakeBuffer();
    const Span s = MakeSpan(0, 0, 3, 5);
    blitvis::PaintSpans(px, kW, kH, &s, 1, &ids);
    ASSERT_EQ(blitvis::BlitIdAt(ids, kW, kH, 0, 0), 5u);

    for (int i = 0; i < 32 && AlphaAt(px, 0, 0) != 0; ++i) {
        blitvis::DecayPixels(px, 4, &ids);
        if (AlphaAt(px, 0, 0) != 0)
            EXPECT_EQ(blitvis::BlitIdAt(ids, kW, kH, 0, 0), 5u) << "still visible, still owned";
    }

    ASSERT_EQ(AlphaAt(px, 0, 0), 0u);
    EXPECT_EQ(blitvis::BlitIdAt(ids, kW, kH, 0, 0), 0u);
}

TEST(BlitterVisOverlayIdMap, RejectsOutOfBoundsLookups)
{
    // Screen-to-pixel arithmetic at the edge of the picture can land one past the end.
    auto ids = MakeBuffer();
    EXPECT_EQ(blitvis::BlitIdAt(ids, kW, kH, -1, 0), 0u);
    EXPECT_EQ(blitvis::BlitIdAt(ids, kW, kH, 0, -1), 0u);
    EXPECT_EQ(blitvis::BlitIdAt(ids, kW, kH, kW, 0), 0u);
    EXPECT_EQ(blitvis::BlitIdAt(ids, kW, kH, 0, kH), 0u);
}

// ---- decay --------------------------------------------------------------------------------

TEST(BlitterVisOverlayDecay, FadesToNothingWithinTheConfiguredFrames)
{
    auto px = MakeBuffer();
    const Span s = MakeSpan(0, 0, 3, 1);
    blitvis::PaintSpans(px, kW, kH, &s, 1);
    ASSERT_EQ(AlphaAt(px, 0, 0), 255u);

    constexpr int kFrames = 5;
    bool visible = true;
    for (int i = 0; i < kFrames; ++i)
        visible = blitvis::DecayPixels(px, kFrames);

    EXPECT_FALSE(visible);
    EXPECT_EQ(AlphaAt(px, 0, 0), 0u) << "a one-shot blit should not linger past its decay";
}

TEST(BlitterVisOverlayDecay, IsMonotonicAndPreservesColour)
{
    auto px = MakeBuffer();
    const Span s = MakeSpan(0, 0, 3, 12);
    blitvis::PaintSpans(px, kW, kH, &s, 1);
    const uint32_t rgb = At(px, 0, 0) & 0x00FFFFFFu;

    uint32_t prev = AlphaAt(px, 0, 0);
    for (int i = 0; i < 3; ++i) {
        blitvis::DecayPixels(px, 10);
        const uint32_t now = AlphaAt(px, 0, 0);
        EXPECT_LT(now, prev) << "alpha must strictly decrease while visible";
        EXPECT_EQ(At(px, 0, 0) & 0x00FFFFFFu, rgb) << "the blit's colour must not drift";
        prev = now;
    }
}

TEST(BlitterVisOverlayDecay, LeavesFullyTransparentPixelsAlone)
{
    auto px = MakeBuffer();
    EXPECT_FALSE(blitvis::DecayPixels(px, 5));
    for (uint32_t p : px)
        EXPECT_EQ(p, 0u);
}

TEST(BlitterVisOverlayDecay, ASingleFrameDecayClearsImmediately)
{
    // decayFrames = 1 means "show only the frame it was fetched in".
    auto px = MakeBuffer();
    const Span s = MakeSpan(0, 0, 3, 1);
    blitvis::PaintSpans(px, kW, kH, &s, 1);

    EXPECT_FALSE(blitvis::DecayPixels(px, 1));
    EXPECT_EQ(AlphaAt(px, 0, 0), 0u);
}

// ---- painted-id collection ----------------------------------------------------------------
//
// The overlay's answer to "which blits could I NOT show". A blit in the frame whose id is absent
// from this list was never fetched for display, so the caller falls back to a geometry box for
// it (see Application::DrawBlitOverlay). Getting this wrong in either direction is visible:
// a false positive loses the box and the blit disappears from the overlay entirely, a false
// negative double-draws a box over a fill that is already showing it.

TEST(BlitterVisOverlayPaintedIds, IsEmptyForNoSpans)
{
    std::vector<uint32_t> ids{7, 7, 7}; // deliberately dirty, to prove it is cleared
    blitvis::CollectPaintedIds(nullptr, 0, ids);
    EXPECT_TRUE(ids.empty());

    const Span s = MakeSpan(0, 0, 3, 1);
    blitvis::CollectPaintedIds(&s, 0, ids);
    EXPECT_TRUE(ids.empty());
}

TEST(BlitterVisOverlayPaintedIds, IsSortedAndDeduplicated)
{
    // A blit spans many scanlines, so the same id arrives dozens of times, and the core emits
    // them interleaved rather than grouped.
    const Span spans[] = {
        MakeSpan(0, 0, 3, 9), MakeSpan(0, 1, 3, 4), MakeSpan(0, 2, 3, 9),
        MakeSpan(0, 3, 3, 1), MakeSpan(0, 4, 3, 4),
    };
    std::vector<uint32_t> ids;
    blitvis::CollectPaintedIds(spans, 5, ids);

    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 1u);
    EXPECT_EQ(ids[1], 4u);
    EXPECT_EQ(ids[2], 9u);
}

TEST(BlitterVisOverlayPaintedIds, SkipsTheNoBlitSentinel)
{
    // Id 0 means "no blit"; it must never come back as something that was painted, or every
    // blit would look like it had been shown.
    const Span spans[] = {MakeSpan(0, 0, 3, 0), MakeSpan(0, 1, 3, 5), MakeSpan(0, 2, 3, 0)};
    std::vector<uint32_t> ids;
    blitvis::CollectPaintedIds(spans, 3, ids);

    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 5u);
}

TEST(BlitterVisOverlayPaintedIds, SupportsBinarySearchLookup)
{
    // This is how BlitterVisOverlay::HasSpansThisFrame consumes the list.
    const Span spans[] = {MakeSpan(0, 0, 3, 30), MakeSpan(0, 1, 3, 10), MakeSpan(0, 2, 3, 20)};
    std::vector<uint32_t> ids;
    blitvis::CollectPaintedIds(spans, 3, ids);

    EXPECT_TRUE(std::binary_search(ids.begin(), ids.end(), 10u));
    EXPECT_TRUE(std::binary_search(ids.begin(), ids.end(), 20u));
    EXPECT_TRUE(std::binary_search(ids.begin(), ids.end(), 30u));
    EXPECT_FALSE(std::binary_search(ids.begin(), ids.end(), 15u));
    EXPECT_FALSE(std::binary_search(ids.begin(), ids.end(), 0u));
}

TEST(BlitterVisOverlayPaintedIds, AgreesWithWhatPaintSpansActuallyDrew)
{
    // The two must not drift: whatever PaintSpans put into the id map is exactly what
    // CollectPaintedIds should report, clipping aside.
    auto px = MakeBuffer();
    std::vector<uint32_t> idMap(px.size(), 0u);
    const Span spans[] = {MakeSpan(0, 0, 3, 11), MakeSpan(8, 1, 15, 22)};

    blitvis::PaintSpans(px, kW, kH, spans, 2, &idMap);

    std::vector<uint32_t> ids;
    blitvis::CollectPaintedIds(spans, 2, ids);

    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(blitvis::BlitIdAt(idMap, kW, kH, 0, 0), ids[0]);
    EXPECT_EQ(blitvis::BlitIdAt(idMap, kW, kH, 8, 1), ids[1]);
}

} // namespace
