// Tests for the blitter destination provenance map that scripts/build_puae.{ps1,sh} inject
// into libretro-core.c (patches/adm_blitvis.c).
//
// That code normally only exists inside the PuAE DLL, which means a bug in it would otherwise
// cost a full core rebuild to find. Compiling it here against a small stand-in for the handful
// of UAE/libretro declarations it touches lets the algorithm — the provenance stamping, the
// fetch-time tagging, the source-to-native projection and the span extraction — be tested
// directly, with no emulator and no rebuild.
//
// If these fail after a PuAE bump, the algorithm is wrong. If these pass but the overlay is
// still misplaced on screen, the problem is in the hook sites (which anchors the build scripts
// patch into custom.c/drawing.c), not in here.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---- stand-ins for the core declarations adm_blitvis.c relies on -------------------------
// Values match sources/src/include/{custom,drawing}.h so the index arithmetic under test is
// identical to the one that ships.
namespace admblitvis_test {

typedef uint32_t uaecptr;
struct AddrBankStub
{
    uint32_t mask;
};

} // namespace admblitvis_test

using admblitvis_test::uaecptr;

static admblitvis_test::AddrBankStub chipmem_bank = {0x1FFFFF}; // 2 MB chip
static unsigned short retrow_crop = 0, retroh_crop = 0, retrox_crop = 0, retroy_crop = 0;

#define MAX_PLANES          8
#define MAXVPOS             800
#define MAXVPOS_WRAPLINES   10
#define MAX_PIXELS_PER_LINE 2304
#define RETRO_API

namespace admblitvis {
#include "../patches/adm_blitvis.c" // NOLINT — deliberately compiled into the test
} // namespace admblitvis

namespace {

using namespace admblitvis;

// drawing.c initialises src_pixel to MAX_PIXELS_PER_LINE + <small offset>, while the fetch
// side counts from 0. Tests that drive both halves must convert between the two.
constexpr int kSrcPixelOrigin = MAX_PIXELS_PER_LINE;

constexpr uint32_t kChipBase = 0x20000;

class AdmBlitVisTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        retrow_crop = 320;
        retroh_crop = 256;
        retrox_crop = 0;
        retroy_crop = 0;
        adm_set_blitter_vis_enabled(1);
        ASSERT_TRUE(adm_blitvis_enabled) << "storage allocation failed";
        adm_blitvis_reset_line_cursors();
    }

    void TearDown() override { adm_set_blitter_vis_enabled(0); }

    // Stamps `words` consecutive chip words as written by the current blit.
    static void RecordWriteRun(uaecptr addr, int words)
    {
        for (int i = 0; i < words; ++i)
            adm_blitvis_record_write(addr + static_cast<uaecptr>(i * 2));
    }

    static std::vector<adm_blitvis_span_t> TakeSpans()
    {
        std::vector<adm_blitvis_span_t> out(256);
        const int n = adm_get_blitter_vis_spans(out.data(), static_cast<int>(out.size()));
        out.resize(static_cast<size_t>(n));
        return out;
    }
};

// ---- stage 1: the provenance map ---------------------------------------------------------

TEST_F(AdmBlitVisTest, BlitIdsAreUniqueAndNonZero)
{
    const uint32_t a = adm_blitvis_begin_blit();
    const uint32_t b = adm_blitvis_begin_blit();
    EXPECT_NE(a, 0u) << "0 is reserved for 'no blit'";
    EXPECT_NE(b, 0u);
    EXPECT_NE(a, b);
}

TEST_F(AdmBlitVisTest, TagIsConsumedByTheFirstFetchOnly)
{
    const uint32_t id = adm_blitvis_begin_blit();
    adm_blitvis_record_write(kChipBase);

    // First fetch of that word tags it...
    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(10, kChipBase, /*plane*/ 0, /*fm*/ 0, /*delay*/ 0, /*pipeline*/ 0);
    int enabled = 0, writes = 0, tags = 0, spans = 0;
    adm_get_blitter_vis_stats(&enabled, &writes, &tags, &spans);

    // ...and a second fetch of the same word does not, so a word re-read later in the frame
    // cannot double-count. Proven via the frame counters, which only advance on a real tag.
    adm_blitvis_frame_begin(); // rolls this-frame counters into last-frame
    adm_get_blitter_vis_stats(&enabled, &writes, &tags, &spans);
    EXPECT_EQ(writes, 1);
    EXPECT_EQ(tags, 1) << "the first fetch should have tagged exactly one word";

    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(10, kChipBase, 0, 0, 0, 0);
    adm_blitvis_frame_begin();
    adm_get_blitter_vis_stats(&enabled, &writes, &tags, &spans);
    EXPECT_EQ(tags, 0) << "the tag should have been consumed by the first fetch";
}

TEST_F(AdmBlitVisTest, ANewWriteRearmsTheTag)
{
    adm_blitvis_begin_blit();
    adm_blitvis_record_write(kChipBase);
    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(10, kChipBase, 0, 0, 0, 0); // consumes it
    adm_blitvis_frame_begin();

    // A later blit writing the same word makes it visible again — this is what keeps a sprite
    // that is re-blitted every frame lit up rather than flashing once.
    adm_blitvis_begin_blit();
    adm_blitvis_record_write(kChipBase);
    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(10, kChipBase, 0, 0, 0, 0);
    adm_blitvis_frame_begin();

    int enabled = 0, writes = 0, tags = 0, spans = 0;
    adm_get_blitter_vis_stats(&enabled, &writes, &tags, &spans);
    EXPECT_EQ(tags, 1);
}

TEST_F(AdmBlitVisTest, UnwrittenMemoryIsNeverTagged)
{
    adm_blitvis_begin_blit();
    adm_blitvis_record_write(kChipBase);

    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(10, kChipBase + 0x1000, 0, 0, 0, 0); // a word the blitter never touched
    adm_blitvis_frame_begin();

    int enabled = 0, writes = 0, tags = 0, spans = 0;
    adm_get_blitter_vis_stats(&enabled, &writes, &tags, &spans);
    EXPECT_EQ(writes, 1);
    EXPECT_EQ(tags, 0);
}

TEST_F(AdmBlitVisTest, DisabledCoreRecordsNothing)
{
    adm_set_blitter_vis_enabled(0);
    adm_blitvis_begin_blit();
    adm_blitvis_record_write(kChipBase); // must not touch freed tables

    adm_set_blitter_vis_enabled(1);
    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(10, kChipBase, 0, 0, 0, 0);
    adm_blitvis_frame_begin();

    int enabled = 0, writes = 0, tags = 0, spans = 0;
    adm_get_blitter_vis_stats(&enabled, &writes, &tags, &spans);
    EXPECT_EQ(tags, 0) << "writes made while disabled must not resurface once re-enabled";
}

// ---- stage 2: fetch-time pixel accounting ------------------------------------------------

TEST_F(AdmBlitVisTest, FetchAdvancesThePixelCursorPerPlane)
{
    const uint32_t id = adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase, 3);

    adm_blitvis_reset_line_cursors();
    // Three consecutive lores words on plane 0 → source pixels 0..15, 16..31, 32..47.
    adm_blitvis_fetch(5, kChipBase + 0, 0, 0, 0, 0);
    adm_blitvis_fetch(5, kChipBase + 2, 0, 0, 0, 0);
    adm_blitvis_fetch(5, kChipBase + 4, 0, 0, 0, 0);

    // Read them back through the projection at 1:1, which is the only public route into the
    // source grid. Source coords are src_pixel-space, hence the origin offset.
    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 48, 0, 48, 0);
    adm_blitvis_frame_begin();

    const auto spans = TakeSpans();
    ASSERT_EQ(spans.size(), 1u) << "three abutting words of one blit should coalesce";
    EXPECT_EQ(spans[0].y, 0);
    EXPECT_EQ(spans[0].x, 0);
    EXPECT_EQ(spans[0].xEnd, 47);
    EXPECT_EQ(spans[0].blitId, id);
}

TEST_F(AdmBlitVisTest, XScrollDelayShiftsTheTaggedPixels)
{
    const uint32_t id = adm_blitvis_begin_blit();
    // Only the fifth word belongs to the blit; the four before it are untagged memory. The
    // pixel cursor still advances over them, which puts the tagged pixels clear of the segment
    // edges (see EdgeFallbackExtendsTagsTowardsSegmentEdges below).
    RecordWriteRun(kChipBase + 8, 1);

    adm_blitvis_reset_line_cursors();
    // A BPLCON1 scroll of 8 lores pixels: the words still come from the same addresses, but
    // the pixels they produce land 8 further right. Getting this wrong is exactly the failure
    // mode the geometry-based overlay has on scrolling playfields.
    for (int i = 0; i < 10; ++i)
        adm_blitvis_fetch(5, kChipBase + static_cast<uaecptr>(i * 2), 0, 0, /*delay*/ 8, 0);

    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 320, 0, 320, 0);
    adm_blitvis_frame_begin();

    const auto spans = TakeSpans();
    ASSERT_EQ(spans.size(), 1u);
    // Word 4 covers source pixels 64..79; the 8-pixel scroll moves it to 72..87.
    EXPECT_EQ(spans[0].x, 72);
    EXPECT_EQ(spans[0].xEnd, 87);
    EXPECT_EQ(spans[0].blitId, id);
}

// Within 32 native pixels of either end of a linetoscr segment the projection widens its
// search by 64 source pixels, because the fetch that produced an edge pixel can sit just
// outside the range the source/native ratio computes. Transplanted from upstream verbatim.
//
// The visible consequence is that a blit ending near a segment edge bleeds a little way
// towards it. Pinning it here means a future change to the fallback shows up as a test
// failure rather than as a puzzling smear at the edge of the picture.
TEST_F(AdmBlitVisTest, EdgeFallbackExtendsTagsTowardsSegmentEdges)
{
    adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase, 1);

    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(5, kChipBase, 0, 0, /*delay*/ 8, 0); // tags source pixels 8..23

    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 64, 0, 64, 0);
    adm_blitvis_frame_begin();

    const auto spans = TakeSpans();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].x, 8);
    EXPECT_EQ(spans[0].xEnd, 31)
        << "pixels 24..31 are within 32 of the left edge, so the fallback finds the tag to "
           "their left; past 32 the bleed stops";
}

TEST_F(AdmBlitVisTest, AgaFetchModeCoversFourWordsOfPixels)
{
    const uint32_t id = adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase, 4);

    adm_blitvis_reset_line_cursors();
    // fm = 2 → a 64-bit fetch, four chip words, 64 source pixels in one go.
    adm_blitvis_fetch(5, kChipBase, 0, /*fm*/ 2, 0, 0);

    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 64, 0, 64, 0);
    adm_blitvis_frame_begin();

    const auto spans = TakeSpans();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].x, 0);
    EXPECT_EQ(spans[0].xEnd, 63);
    EXPECT_EQ(spans[0].blitId, id);
}

TEST_F(AdmBlitVisTest, LongFetchCoalescesConsecutiveWords)
{
    const uint32_t id = adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase, 4);

    adm_blitvis_reset_line_cursors();
    adm_blitvis_long_fetch(5, kChipBase, /*plane*/ 0, /*nwords*/ 4, /*delay*/ 0);

    // long_fetch applies a fixed pipeline advance of 16, matching upstream.
    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 128, 0, 128, 0);
    adm_blitvis_frame_begin();

    const auto spans = TakeSpans();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].x, 16);
    EXPECT_EQ(spans[0].xEnd, 79) << "four words = 64 pixels, offset by the pipeline advance";
    EXPECT_EQ(spans[0].blitId, id);
}

TEST_F(AdmBlitVisTest, SeparateBlitsProduceSeparateSpans)
{
    const uint32_t first = adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase, 1);
    const uint32_t second = adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase + 2, 1);

    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(5, kChipBase + 0, 0, 0, 0, 0);
    adm_blitvis_fetch(5, kChipBase + 2, 0, 0, 0, 0);

    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 32, 0, 32, 0);
    adm_blitvis_frame_begin();

    const auto spans = TakeSpans();
    ASSERT_EQ(spans.size(), 2u) << "adjacent words from different blits must not coalesce";
    EXPECT_EQ(spans[0].blitId, first);
    EXPECT_EQ(spans[0].x, 0);
    EXPECT_EQ(spans[0].xEnd, 15);
    EXPECT_EQ(spans[1].blitId, second);
    EXPECT_EQ(spans[1].x, 16);
    EXPECT_EQ(spans[1].xEnd, 31);
}

// ---- stage 3: projection to native output ------------------------------------------------

TEST_F(AdmBlitVisTest, ProjectionScalesSourceOntoNative)
{
    const uint32_t id = adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase, 2);

    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(5, kChipBase + 0, 0, 0, 0, 0);
    adm_blitvis_fetch(5, kChipBase + 2, 0, 0, 0, 0);

    // 32 source pixels rendered into 64 native pixels — the hires-texture case, where the
    // geometry overlay historically halved or doubled the box.
    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 32, 0, 64, 0);
    adm_blitvis_frame_begin();

    const auto spans = TakeSpans();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].x, 0);
    EXPECT_EQ(spans[0].xEnd, 63) << "32 source pixels doubled should cover all 64 native ones";
}

TEST_F(AdmBlitVisTest, CropOffsetsShiftNativeCoordinates)
{
    const uint32_t id = adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase, 2);

    // The core hands the front end a cropped frame; spans must be in that cropped space, since
    // the overlay texture is sized to the visible output.
    retrox_crop = 8;
    retroy_crop = 2;

    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(5, kChipBase + 0, 0, 0, 0, 0);
    adm_blitvis_fetch(5, kChipBase + 2, 0, 0, 0, 0);

    adm_blitvis_begin_line(5, /*native line*/ 10);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 32, 0, 32, 0);
    adm_blitvis_frame_begin();

    const auto spans = TakeSpans();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].y, 8) << "native line 10 minus a 2-line top crop";
    EXPECT_EQ(spans[0].x, 0) << "the left 8 pixels are cropped away and clip to 0";
    EXPECT_EQ(spans[0].xEnd, 23);
}

TEST_F(AdmBlitVisTest, XAdjustShiftsSpansLeft)
{
    adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase, 2);

    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(5, kChipBase + 0, 0, 0, 0, 0);
    adm_blitvis_fetch(5, kChipBase + 2, 0, 0, 0, 0);

    // linetoscr_x_adjust_pixels is the drawing buffer's left-border offset; spans are stored
    // relative to the visible area, so it is subtracted.
    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 32, 40, 72, /*x_adjust*/ 40);
    adm_blitvis_frame_begin();

    const auto spans = TakeSpans();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].x, 0);
    EXPECT_EQ(spans[0].xEnd, 31);
}

TEST_F(AdmBlitVisTest, SpansAreClippedToTheVisibleWidth)
{
    adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase, 4);

    retrow_crop = 40; // a deliberately narrow visible area

    adm_blitvis_reset_line_cursors();
    for (int i = 0; i < 4; ++i)
        adm_blitvis_fetch(5, kChipBase + static_cast<uaecptr>(i * 2), 0, 0, 0, 0);

    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 64, 0, 64, 0);
    adm_blitvis_frame_begin();

    const auto spans = TakeSpans();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].x, 0);
    EXPECT_EQ(spans[0].xEnd, 39) << "must not run past the visible width";
}

// ---- line resolution ---------------------------------------------------------------------

TEST_F(AdmBlitVisTest, AsPreviousLineReusesTheEarlierLinesTags)
{
    const uint32_t id = adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase, 2);

    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(/*lineno*/ 5, kChipBase + 0, 0, 0, 0, 0);
    adm_blitvis_fetch(/*lineno*/ 5, kChipBase + 2, 0, 0, 0, 0);

    // Line 5 is drawn normally into native row 0...
    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 32, 0, 32, 0);

    // ...and line 6 is LINE_AS_PREVIOUS, re-displaying line 5's decoded data. Its tags live
    // under line 5's index, so without the resolution step this row would come out blank.
    adm_blitvis_note_as_previous();
    adm_blitvis_begin_line(6, 1);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 32, 0, 32, 0);

    adm_blitvis_frame_begin();

    const auto spans = TakeSpans();
    ASSERT_EQ(spans.size(), 2u) << "both native rows should be covered";
    EXPECT_EQ(spans[0].y, 0);
    EXPECT_EQ(spans[1].y, 1);
    EXPECT_EQ(spans[1].blitId, id);
}

// ---- frame lifecycle ---------------------------------------------------------------------

TEST_F(AdmBlitVisTest, SpansDoNotLeakIntoTheFollowingFrame)
{
    adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase, 2);

    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(5, kChipBase + 0, 0, 0, 0, 0);
    adm_blitvis_fetch(5, kChipBase + 2, 0, 0, 0, 0);
    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 32, 0, 32, 0);
    adm_blitvis_frame_begin();
    ASSERT_EQ(TakeSpans().size(), 1u);

    // A frame in which nothing is tagged must clear the native grid, or a one-shot blit would
    // stay burned into the overlay forever.
    adm_blitvis_frame_begin();
    EXPECT_TRUE(TakeSpans().empty());
}

TEST_F(AdmBlitVisTest, ResetClearsTheProvenanceMap)
{
    adm_blitvis_begin_blit();
    adm_blitvis_record_write(kChipBase);

    // Chip RAM has been replaced wholesale (reset / savestate restore): the old stamps describe
    // memory that no longer exists.
    adm_blitvis_reset();

    adm_blitvis_reset_line_cursors();
    adm_blitvis_fetch(5, kChipBase, 0, 0, 0, 0);
    adm_blitvis_frame_begin();

    int enabled = 0, writes = 0, tags = 0, spans = 0;
    adm_get_blitter_vis_stats(&enabled, &writes, &tags, &spans);
    EXPECT_EQ(tags, 0);
}

TEST_F(AdmBlitVisTest, StatsReportWhetherTheMapIsArmed)
{
    int enabled = 0, writes = 0, tags = 0, spans = 0;
    adm_get_blitter_vis_stats(&enabled, &writes, &tags, &spans);
    EXPECT_EQ(enabled, 1);

    adm_set_blitter_vis_enabled(0);
    adm_get_blitter_vis_stats(&enabled, &writes, &tags, &spans);
    EXPECT_EQ(enabled, 0);
}

// A blit into a buffer that no BPLxPT ever points at produces nothing. This is the documented
// limit of the technique, not a bug — it is why the geometry-based overlay is kept alongside.
TEST_F(AdmBlitVisTest, OffscreenBlitProducesNoSpans)
{
    adm_blitvis_begin_blit();
    RecordWriteRun(kChipBase + 0x10000, 16); // a scratch buffer, never fetched for display

    adm_blitvis_begin_line(5, 0);
    adm_blitvis_project(kSrcPixelOrigin, kSrcPixelOrigin + 256, 0, 256, 0);
    adm_blitvis_frame_begin();

    EXPECT_TRUE(TakeSpans().empty());

    int enabled = 0, writes = 0, tags = 0, spans = 0;
    adm_get_blitter_vis_stats(&enabled, &writes, &tags, &spans);
    EXPECT_EQ(writes, 16) << "the writes were still recorded...";
    EXPECT_EQ(tags, 0) << "...but nothing ever fetched them for display";
}

} // namespace
