#include <gtest/gtest.h>

#include "../src/core/BlitScreenMapper.h"

using namespace adm::core;

namespace {

// A simple 320x256 single-plane lores layout with a 1:1 screen map (hbase/vbase = 0),
// so normalized coords are just hpix/320 and vscan/256.
BitplaneLayout MakeLayout(uint32_t base = 0x10000)
{
    BitplaneLayout l;
    l.bplCount = 1;
    l.screenWidth = 320;
    l.screenHeight = 256;
    l.modulo1 = 0;
    l.modulo2 = 0;
    l.interleaved = false;
    l.bplBase[0] = base;
    l.ddfStartLoresPx = 112; // (0x38 & 0xFE) * 2
    l.diwVStart = 44;
    l.valid = true;
    return l;
}

AmigaScreenMap MakeUnitMap()
{
    AmigaScreenMap m;
    m.loresScale = 1.0f;
    m.vScale = 1.0f;
    m.hbase = 0;
    m.vbase = 0;
    m.loresTexW = 320.0f;
    m.scanTexH = 256.0f;
    m.valid = true;
    return m;
}

// A hires-width texture (loresScale=2) showing a 320-lores-wide frame: loresTexW = 640/2 = 320.
// Used to prove the blit box keys its horizontal scale off the bitplane resolution, not the
// texture resolution.
AmigaScreenMap MakeHiresTextureMap()
{
    AmigaScreenMap m;
    m.loresScale = 2.0f;
    m.vScale = 1.0f;
    m.hbase = 0;
    m.vbase = 0;
    m.loresTexW = 320.0f; // 640 texture px / 2
    m.scanTexH = 256.0f;
    m.valid = true;
    return m;
}

BlitRecord MakeBlit(uint32_t dst, uint16_t rows, uint16_t words)
{
    BlitRecord r{};
    r.useD = true;
    r.rows = rows;
    r.wordsPerRow = words;
    r.raw.bltdpt = dst;
    r.raw.bltcon1 = 0;
    r.raw.bltdmod = 0;
    return r;
}

constexpr float kEps = 0.001f;

} // namespace

TEST(BlitScreenMapper, FrontBufferMapsToExpectedRect)
{
    auto layout = MakeLayout();
    auto map = MakeUnitMap();

    // row 10, byte-column 4 within a 40-byte stride: 0x10000 + 10*40 + 4
    auto blit = MakeBlit(0x10000 + 10 * 40 + 4, /*rows=*/8, /*words=*/2);
    auto box = MapBlitDestToScreen(blit, layout, map);

    ASSERT_TRUE(box.valid);
    EXPECT_EQ(box.kind, BlitBufferKind::Front);
    EXPECT_EQ(box.plane, 0);

    // hpix = 112 + (4*8) = 144 ; width = 2*16 = 32
    EXPECT_NEAR(box.u0, 144.0f / 320.0f, kEps);
    EXPECT_NEAR(box.u1, 176.0f / 320.0f, kEps);
    // vscan = 44 + 10 = 54 ; height = 8
    EXPECT_NEAR(box.v0, 54.0f / 256.0f, kEps);
    EXPECT_NEAR(box.v1, 62.0f / 256.0f, kEps);
}

TEST(BlitScreenMapper, LoresBitplaneInHiresTextureIsNotHalved)
{
    // Regression: a lores bitplane rendered into a hires-width texture (loresScale=2). The box
    // must use the bitplane's resolution, not the texture's, so width/position are NOT halved.
    auto layout = MakeLayout();
    layout.resolution = BitplaneResolution::Lores;
    auto map = MakeHiresTextureMap();

    auto blit = MakeBlit(0x10000 + 10 * 40 + 4, /*rows=*/8, /*words=*/2);
    auto box = MapBlitDestToScreen(blit, layout, map);

    ASSERT_TRUE(box.valid);
    // hpix = 112 + 4*8 = 144 ; width = 2*16 = 32 lores px → over a 320-lores texture.
    EXPECT_NEAR(box.u0, 144.0f / 320.0f, kEps);
    EXPECT_NEAR(box.u1, 176.0f / 320.0f, kEps);
    EXPECT_NEAR(box.u1 - box.u0, 32.0f / 320.0f, kEps); // full width, not 16/320
}

TEST(BlitScreenMapper, HiresBitplaneHalvesPixelWidth)
{
    // A genuine hires bitplane: each bitplane pixel is half a lores pixel wide.
    auto layout = MakeLayout();
    layout.resolution = BitplaneResolution::Hires;
    auto map = MakeHiresTextureMap();

    auto blit = MakeBlit(0x10000 + 10 * 40 + 4, /*rows=*/8, /*words=*/2);
    auto box = MapBlitDestToScreen(blit, layout, map);

    ASSERT_TRUE(box.valid);
    // ax = 4*8*0.5 = 16 → hpix = 128 ; width = 2*16*0.5 = 16 lores px.
    EXPECT_NEAR(box.u0, 128.0f / 320.0f, kEps);
    EXPECT_NEAR(box.u1 - box.u0, 16.0f / 320.0f, kEps);
}

TEST(BlitScreenMapper, SuperHiresBitplaneQuartersPixelWidth)
{
    // Super-hires (BPLCON0 SHRES): each bitplane pixel is a quarter of a lores pixel. Previously
    // the mapper only knew "hires or not" and drew these boxes at double their true width.
    auto layout = MakeLayout();
    layout.resolution = BitplaneResolution::SuperHires;
    auto map = MakeHiresTextureMap();

    auto blit = MakeBlit(0x10000 + 10 * 40 + 4, /*rows=*/8, /*words=*/2);
    auto box = MapBlitDestToScreen(blit, layout, map);

    ASSERT_TRUE(box.valid);
    // ax = 4*8*0.25 = 8 → hpix = 120 ; width = 2*16*0.25 = 8 lores px.
    EXPECT_NEAR(box.u0, 120.0f / 320.0f, kEps);
    EXPECT_NEAR(box.u1 - box.u0, 8.0f / 320.0f, kEps);
}

TEST(BlitScreenMapper, DescendingModeNormalizesToBufferStart)
{
    auto layout = MakeLayout();
    auto map = MakeUnitMap();

    const uint16_t rows = 8, words = 2;
    const uint32_t startAddr = 0x10000 + 10 * 40 + 4;
    // bltdpt for a descending blit points at the end of the block.
    const uint32_t rowStep = words * 2u + 0u;
    const uint32_t endAddr = startAddr + (rows - 1) * rowStep + (words * 2u - 2u);

    auto blit = MakeBlit(endAddr, rows, words);
    blit.raw.bltcon1 = 0x0002; // DESC

    auto box = MapBlitDestToScreen(blit, layout, map);
    ASSERT_TRUE(box.valid);
    EXPECT_EQ(box.kind, BlitBufferKind::Front);
    EXPECT_NEAR(box.u0, 144.0f / 320.0f, kEps);
    EXPECT_NEAR(box.v0, 54.0f / 256.0f, kEps);
}

TEST(BlitScreenMapper, ContiguousBackBufferDetected)
{
    auto layout = MakeLayout();
    auto map = MakeUnitMap();

    const uint32_t span = 40u * 256u; // stride * height
    auto blit = MakeBlit(0x10000 + span + 10 * 40 + 4, /*rows=*/8, /*words=*/2);

    auto box = MapBlitDestToScreen(blit, layout, map);
    ASSERT_TRUE(box.valid);
    EXPECT_EQ(box.kind, BlitBufferKind::Back);
    // Same in-buffer position → same on-screen rect as the front-buffer case.
    EXPECT_NEAR(box.u0, 144.0f / 320.0f, kEps);
    EXPECT_NEAR(box.v0, 54.0f / 256.0f, kEps);
}

TEST(BlitScreenMapper, UnrelatedAddressDoesNotMatch)
{
    auto layout = MakeLayout();
    auto map = MakeUnitMap();

    // Far above any plausible buffer span.
    auto blit = MakeBlit(0x80000, /*rows=*/8, /*words=*/2);
    auto box = MapBlitDestToScreen(blit, layout, map);
    EXPECT_FALSE(box.valid);
}

TEST(BlitScreenMapper, ModuloGapColumnIsRejected)
{
    auto layout = MakeLayout();
    layout.modulo1 = 8; // stride becomes 48; bytes-per-row stays 40
    auto map = MakeUnitMap();

    // colByte = 44 lands in the 40..47 modulo gap → not a visible column.
    auto blit = MakeBlit(0x10000 + 10 * 48 + 44, /*rows=*/8, /*words=*/2);
    auto box = MapBlitDestToScreen(blit, layout, map);
    EXPECT_FALSE(box.valid);
}

TEST(BlitScreenMapper, RejectsBlitWithoutDestinationChannel)
{
    auto layout = MakeLayout();
    auto map = MakeUnitMap();

    auto blit = MakeBlit(0x10000 + 10 * 40 + 4, /*rows=*/8, /*words=*/2);
    blit.useD = false;
    auto box = MapBlitDestToScreen(blit, layout, map);
    EXPECT_FALSE(box.valid);
}

TEST(BlitDestToPlane, FrontBufferMapsToPlaneLocalRect)
{
    auto layout = MakeLayout();
    // row 10, byte-column 4 → x = 32, width = 32 ; y = 10, height = 8 (plane-local, no border)
    auto blit = MakeBlit(0x10000 + 10 * 40 + 4, /*rows=*/8, /*words=*/2);
    auto box = MapBlitDestToPlane(blit, layout);

    ASSERT_TRUE(box.valid);
    EXPECT_EQ(box.kind, BlitBufferKind::Front);
    EXPECT_EQ(box.plane, 0);
    EXPECT_NEAR(box.u0, 32.0f / 320.0f, kEps);
    EXPECT_NEAR(box.u1, 64.0f / 320.0f, kEps);
    EXPECT_NEAR(box.v0, 10.0f / 256.0f, kEps);
    EXPECT_NEAR(box.v1, 18.0f / 256.0f, kEps);
}

TEST(BlitDestToPlane, BackBufferIsNotDrawnOnPlaneTile)
{
    auto layout = MakeLayout();
    const uint32_t span = 40u * 256u;
    auto blit = MakeBlit(0x10000 + span + 10 * 40 + 4, /*rows=*/8, /*words=*/2);
    // A back-buffer write doesn't belong on the front-buffer tile → invalid.
    EXPECT_FALSE(MapBlitDestToPlane(blit, layout).valid);
}

TEST(BlitDestToPlane, ReportsSecondPlaneIndex)
{
    auto layout = MakeLayout();
    layout.bplCount = 2;
    layout.bplBase[1] = 0x20000;
    auto blit = MakeBlit(0x20000 + 5 * 40 + 0, /*rows=*/4, /*words=*/1);
    auto box = MapBlitDestToPlane(blit, layout);
    ASSERT_TRUE(box.valid);
    EXPECT_EQ(box.plane, 1);
}

TEST(AmigaScreenMap, BuildDetectsHiresAndCrop)
{
    adm::AmigaEmulator::DisplayGeometry geom;
    geom.fullW = 720; // > 400 → hires
    geom.fullH = 287;
    geom.cropX = 0;
    geom.cropY = 0;
    geom.visibleLeftBorder = 0;
    geom.frameYAdjust = 0;

    auto m = AmigaScreenMap::Build(geom, 720, 287);
    ASSERT_TRUE(m.valid);
    EXPECT_FLOAT_EQ(m.loresScale, 2.0f);
    EXPECT_FLOAT_EQ(m.loresTexW, 360.0f); // 720 / 2
}

// ---- picking a box off the picture ----------------------------------------------------------
//
// The geometry half of the blit overlay's hit-test (Application::PickBlit). The provenance id map
// answers first because it is exact; these boxes are the fallback, and the only way a blit into a
// buffer that is never scanned out can be clicked at all.

namespace {

BlitOverlayBox MakeBox(float u0, float v0, float u1, float v1, bool valid = true)
{
    BlitOverlayBox b;
    b.valid = valid;
    b.kind = BlitBufferKind::Front;
    b.u0 = u0;
    b.v0 = v0;
    b.u1 = u1;
    b.v1 = v1;
    return b;
}

} // namespace

TEST(BlitScreenMapperPick, ReturnsNoneForAnEmptyList)
{
    EXPECT_EQ(PickSmallestBoxContaining(nullptr, 0, 0.5f, 0.5f), -1);
}

TEST(BlitScreenMapperPick, ReturnsNoneWhenThePointMissesEveryBox)
{
    const BlitOverlayBox boxes[] = {
        MakeBox(0.0f, 0.0f, 0.2f, 0.2f),
        MakeBox(0.8f, 0.8f, 1.0f, 1.0f),
    };
    EXPECT_EQ(PickSmallestBoxContaining(boxes, 2, 0.5f, 0.5f), -1);
}

TEST(BlitScreenMapperPick, SkipsInvalidBoxes)
{
    // A blit whose destination could not be placed produces an invalid box; it must never be
    // selectable, or clicking empty space would pin an unrelated blit.
    const BlitOverlayBox boxes[] = {
        MakeBox(0.0f, 0.0f, 1.0f, 1.0f, /*valid=*/false),
        MakeBox(0.4f, 0.4f, 0.6f, 0.6f),
    };
    EXPECT_EQ(PickSmallestBoxContaining(boxes, 2, 0.5f, 0.5f), 1);
    EXPECT_EQ(PickSmallestBoxContaining(boxes, 2, 0.05f, 0.05f), -1);
}

TEST(BlitScreenMapperPick, PrefersTheSmallestContainingBox)
{
    // The case this rule exists for: a full-screen clear and the sprite blitted on top of it
    // both contain the point, and the small one is the one worth selecting.
    const BlitOverlayBox boxes[] = {
        MakeBox(0.0f, 0.0f, 1.0f, 1.0f), // full-screen clear
        MakeBox(0.4f, 0.4f, 0.6f, 0.6f), // sprite on top
        MakeBox(0.0f, 0.0f, 0.8f, 0.8f), // a mid-sized blit, also containing the point
    };
    EXPECT_EQ(PickSmallestBoxContaining(boxes, 3, 0.5f, 0.5f), 1);
}

TEST(BlitScreenMapperPick, FallsBackToTheLargerBoxOutsideTheSmallOne)
{
    const BlitOverlayBox boxes[] = {
        MakeBox(0.0f, 0.0f, 1.0f, 1.0f),
        MakeBox(0.4f, 0.4f, 0.6f, 0.6f),
    };
    EXPECT_EQ(PickSmallestBoxContaining(boxes, 2, 0.1f, 0.1f), 0);
}

TEST(BlitScreenMapperPick, TreatsEdgesAsInside)
{
    // Boxes can be a single scanline tall (a one-row blit), so an exclusive edge test would
    // make them unclickable.
    const BlitOverlayBox boxes[] = {MakeBox(0.25f, 0.5f, 0.75f, 0.5f)};
    EXPECT_EQ(PickSmallestBoxContaining(boxes, 1, 0.25f, 0.5f), 0);
    EXPECT_EQ(PickSmallestBoxContaining(boxes, 1, 0.75f, 0.5f), 0);
    EXPECT_EQ(PickSmallestBoxContaining(boxes, 1, 0.5f, 0.5f), 0);
    EXPECT_EQ(PickSmallestBoxContaining(boxes, 1, 0.76f, 0.5f), -1);
}

TEST(BlitScreenMapperPick, HandlesAnInvertedBox)
{
    // Nothing in the pipeline emits u1 < u0 today, but a hit-test that silently returned "miss"
    // for one would be a nasty thing to debug later.
    const BlitOverlayBox boxes[] = {MakeBox(0.6f, 0.6f, 0.4f, 0.4f)};
    EXPECT_EQ(PickSmallestBoxContaining(boxes, 1, 0.5f, 0.5f), 0);
}

TEST(BlitScreenMapperPick, WorksOnBoxesProducedByMapBlitDestToScreen)
{
    // End-to-end: a real mapped destination must be pickable at its own centre. Guards against
    // the two halves disagreeing about coordinate space.
    BitplaneLayout layout = MakeLayout();
    AmigaScreenMap map = MakeUnitMap();
    BlitRecord rec = MakeBlit(layout.bplBase[0] + 40u * 8u, /*rows=*/16, /*words=*/2);

    BlitOverlayBox box = MapBlitDestToScreen(rec, layout, map);
    ASSERT_TRUE(box.valid);

    const float cu = (box.u0 + box.u1) * 0.5f;
    const float cv = (box.v0 + box.v1) * 0.5f;
    EXPECT_EQ(PickSmallestBoxContaining(&box, 1, cu, cv), 0);

    // And not pickable well outside it.
    EXPECT_EQ(PickSmallestBoxContaining(&box, 1, box.u0 - 0.05f, cv), -1);
}
