#include <gtest/gtest.h>

#include <vector>

#include "core/CopperRaster.h"
#include "core/RasterFrameMap.h"

using namespace adm::core;
using copraster::CopClass;
using DmaSlot = adm::AmigaEmulator::DmaSlotRecord;
using CopEvent = adm::AmigaEmulator::CopperExecEvent;

namespace {

constexpr int kHpos = 228;
constexpr int kVpos = 40; // a short frame keeps the fixtures small

// The DMA_EVENT_* bits the rasteriser reads, mirrored from PuAE's debug.h.
constexpr uint32_t kEvtCopperWake = 16;
constexpr uint32_t kEvtCopperWanted = 128;
constexpr uint32_t kEvtDdfStrt = 4096;
constexpr uint32_t kEvtHbs = 0x00100000;
constexpr uint32_t kEvtHbe = 0x00200000;
constexpr uint32_t kEvtLol = 0x00080000;
constexpr uint32_t kEvtHdiwe = 0x00800000;

// A screen map standing in for a typical PAL lores frame: the texture starts partway into the
// line and partway down the frame, which is what leaves room for the blanking margins.
AmigaScreenMap MakeScreenMap(int hbase = 88, int vbase = 26, float texW = 320.0f,
                             float texH = 256.0f)
{
    AmigaScreenMap m;
    m.loresScale = 1.0f;
    m.vScale = 1.0f;
    m.hbase = hbase;
    m.vbase = vbase;
    m.loresTexW = texW;
    m.scanTexH = texH;
    m.valid = true;
    return m;
}

std::vector<DmaSlot> MakeRecords(int hposCount = kHpos, int vposCount = kVpos)
{
    std::vector<DmaSlot> r(static_cast<size_t>(hposCount) * vposCount);
    for (int v = 0; v < vposCount; ++v) {
        for (int h = 0; h < hposCount; ++h) {
            auto& s = r[static_cast<size_t>(v) * hposCount + h];
            s.vpos = v;
            s.hpos = h;
            s.type = 0;
        }
    }
    return r;
}

DmaSlot& SlotAt(std::vector<DmaSlot>& r, int v, int h, int hposCount = kHpos)
{
    return r[static_cast<size_t>(v) * hposCount + h];
}

copraster::Palette TestPalette()
{
    copraster::Palette p = copraster::Palette::Default();
    // Distinct, fully-opaque channel colours so a composite result is unambiguous.
    for (int i = 0; i < 10; ++i)
        p.channel[i] = copraster::PackRGBA(static_cast<uint32_t>(i * 20 + 10), 0, 0, 255);
    return p;
}

copraster::Options QuietOptions()
{
    copraster::Options o;
    // Every layer stated explicitly rather than inherited from the shipping defaults, so each
    // case exercises exactly one pass and a change of default cannot silently rewrite a test.
    o.showCopper = true;
    o.channelMask = 0x03FE;
    o.showBlanking = false;
    o.showGeometryEvents = false;
    o.showWaitSpans = false;
    o.showCopperWanted = false;
    return o;
}

} // namespace

// --- Classification -------------------------------------------------------------------------

TEST(CopperRasterClassify, WaitAndSkipAreDistinguished)
{
    // IR1 bit 0 set => WAIT/SKIP; IR2 bit 0 picks between them.
    EXPECT_EQ(copraster::Classify(0x2C01, 0xFF00), CopClass::Wait);
    EXPECT_EQ(copraster::Classify(0x2C01, 0xFF01), CopClass::Skip);
}

TEST(CopperRasterClassify, MoveIsGroupedByDestinationRegister)
{
    EXPECT_EQ(copraster::Classify(0x0180, 0x0F00), CopClass::MoveColor);   // COLOR00
    EXPECT_EQ(copraster::Classify(0x01BE, 0x0000), CopClass::MoveColor);   // COLOR31
    EXPECT_EQ(copraster::Classify(0x00E0, 0x0002), CopClass::MoveBplPtr);  // BPL1PTH
    EXPECT_EQ(copraster::Classify(0x0120, 0x0002), CopClass::MoveSprite);  // SPR0PTH
    EXPECT_EQ(copraster::Classify(0x0040, 0x0000), CopClass::MoveBlitter); // BLTCON0
    EXPECT_EQ(copraster::Classify(0x0100, 0x0200), CopClass::MoveOther);   // BPLCON0
    EXPECT_EQ(copraster::Classify(0x0096, 0x8200), CopClass::MoveOther);   // DMACON
}

TEST(CopperRasterClassify, EndOfCopperlistIsAWait)
{
    // The canonical FFFF/FFFE terminator waits forever rather than skipping.
    EXPECT_EQ(copraster::Classify(0xFFFF, 0xFFFE), CopClass::Wait);
}

// --- RasterFrameMap -------------------------------------------------------------------------

TEST(RasterFrameMapTest, ImageIsInsetInsideTheFullRaster)
{
    const auto map = RasterFrameMap::Build(MakeScreenMap(), kHpos, 313);
    ASSERT_TRUE(map.valid);
    EXPECT_EQ(map.rasterCC, 227);
    EXPECT_EQ(map.rasterLines, 313);

    // One colour clock is two lores pixels, so a 320-lores-pixel texture spans 160 clocks. It
    // starts at Denise clock 44, which the beam drew at clock 48 (Denise trails it by 4) --
    // leaving real margins on both sides for the blanking slots.
    EXPECT_FLOAT_EQ(map.imageLeftCC, 48.0f);
    EXPECT_FLOAT_EQ(map.imageWidthCC, 160.0f);
    EXPECT_FLOAT_EQ(map.imageTopLine, 26.0f);
    EXPECT_FLOAT_EQ(map.imageHeightLines, 256.0f);

    EXPECT_GT(map.ImageU0(), 0.0f);
    EXPECT_LT(map.ImageU1(), 1.0f);
    EXPECT_GT(map.ImageV0(), 0.0f);
    EXPECT_LT(map.ImageV1(), 1.0f);
}

TEST(RasterFrameMapTest, ClampsRecordPaddingToTheRealLineLength)
{
    // PuAE's record buffer is NR_DMA_REC_HPOS = 288 wide; a PAL line has 227 clocks and the
    // slots past them would otherwise stretch the grid across the screen.
    const auto map = RasterFrameMap::Build(MakeScreenMap(), 288, 313);
    ASSERT_TRUE(map.valid);
    EXPECT_EQ(map.rasterCC, 227);
}

TEST(RasterFrameMapTest, WidensTheRasterWhenThePictureOverrunsTheAgnusLine)
{
    // The real PAL wide-overscan case: PuAE right-aligns a 720-pixel hires buffer to the DENISE
    // line, which is `maxhpos + 7` clocks long, so the picture shows Denise clocks 54.5 to 234.5
    // - drawn at beam clocks 58.5 to 238.5. Clamping the raster to the line length left the
    // right-hand end of the emulator texture hanging off the edge of the overlay.
    const auto map = RasterFrameMap::Build(MakeScreenMap(109, 26, 360.0f, 287.0f), 288, 313);
    ASSERT_TRUE(map.valid);

    EXPECT_EQ(map.dataCC, 227); // the Agnus line is still 227 clocks
    EXPECT_EQ(map.rasterCC, 239);
    EXPECT_FLOAT_EQ(map.imageLeftCC, 58.5f);
    EXPECT_FLOAT_EQ(map.imageWidthCC, 180.0f);

    // Which is the point: the whole picture is now inside the raster rect.
    EXPECT_LE(map.ImageU1(), 1.0f);
}

TEST(RasterFrameMapTest, LeavesTheRasterAtTheLineLengthWhenThePictureFits)
{
    const auto map = RasterFrameMap::Build(MakeScreenMap(), kHpos, 313);
    ASSERT_TRUE(map.valid);
    EXPECT_EQ(map.dataCC, 227);
    EXPECT_EQ(map.rasterCC, 227);
}

TEST(RasterFrameMapTest, PlacesThePictureUnderTheBeamClocksThatDrewIt)
{
    // Measured on an OCS A500 at hires (hbase 109): a Copper MOVE COLOR00 executed at hpos 70
    // put the colour edge 46 hires pixels into the frame, i.e. Denise clock 54.5 + 11.5 = 66.
    // On the raster that pixel must sit in column 70, under the MOVE that caused it.
    const auto map = RasterFrameMap::Build(MakeScreenMap(109, 26, 360.0f, 287.0f), 288, 313);
    ASSERT_TRUE(map.valid);
    const float edgeLores = 46.0f / 2.0f; // 720-pixel hires frame: 2 pixels per lores pixel
    EXPECT_FLOAT_EQ(map.imageLeftCC + edgeLores * 0.5f, 70.0f);
}

TEST(RasterFrameMapTest, GrowsTheRasterToThePicturesLastLine)
{
    // A 287-line picture starting at line 26 ends on line 312. A record a line short (a short
    // field, the first frame after a start) must not leave that line hanging below the raster.
    const auto map = RasterFrameMap::Build(MakeScreenMap(109, 26, 360.0f, 287.0f), 288, 312);
    ASSERT_TRUE(map.valid);
    EXPECT_EQ(map.rasterLines, 313);
    EXPECT_LE(map.ImageV1(), 1.0f);
}

TEST(RasterFrameMapTest, RoundTripsAPositionThroughNormalisedSpace)
{
    const auto map = RasterFrameMap::Build(MakeScreenMap(), kHpos, 313);
    ASSERT_TRUE(map.valid);
    EXPECT_EQ(map.UToCC(map.CCToU(100.0f)), 100);
    EXPECT_EQ(map.VToLine(map.LineToV(200.0f)), 200);
}

TEST(RasterFrameMapTest, InvalidInputsProduceAnInvalidMap)
{
    EXPECT_FALSE(RasterFrameMap::Build(MakeScreenMap(), 0, 313).valid);
    EXPECT_FALSE(RasterFrameMap::Build(MakeScreenMap(), kHpos, 0).valid);

    AmigaScreenMap bad = MakeScreenMap();
    bad.valid = false;
    EXPECT_FALSE(RasterFrameMap::Build(bad, kHpos, 313).valid);
}

// --- Build: DMA layer -----------------------------------------------------------------------

TEST(CopperRasterBuild, PaintsDmaSlotsAtTheirBeamPosition)
{
    auto records = MakeRecords();
    SlotAt(records, 10, 50).type = adm::AmigaEmulator::kDmaRecordBitplane;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;

    copraster::Result out;
    const auto map = RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos);
    copraster::Build(in, map, TestPalette(), QuietOptions(), out);

    ASSERT_EQ(out.width, 227);
    ASSERT_EQ(out.height, map.rasterLines);
    EXPECT_EQ(out.dmaCells, 1);

    const auto* cell = out.At(50, 10);
    ASSERT_NE(cell, nullptr);
    EXPECT_EQ(cell->dmaType, adm::AmigaEmulator::kDmaRecordBitplane);
    EXPECT_NE(out.pixels[10 * out.width + 50], 0u);

    // Everything else stays transparent.
    EXPECT_EQ(out.pixels[10 * out.width + 51], 0u);
}

TEST(CopperRasterBuild, WrapColumnsCarryTheNextLinesLeadingClocks)
{
    // With a widened raster the columns past the end of the 227-clock Agnus line stand for the
    // part of the displayed line that Agnus already spends on the next one, so they must show
    // that line's first slots -- and read no record slot of their own. Measured: a MOVE at hpos 8
    // of line 129 coloured line 128 from Denise clock 231 = 227 + 8 - 4, so the wrap starts at 227.
    auto records = MakeRecords();
    SlotAt(records, 11, 3).type = adm::AmigaEmulator::kDmaRecordBitplane;
    SlotAt(records, 10, 227).type = adm::AmigaEmulator::kDmaRecordCpu; // no such clock on PAL
    SlotAt(records, kVpos - 1, 3).type = adm::AmigaEmulator::kDmaRecordBitplane;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;

    copraster::Result out;
    const auto map = RasterFrameMap::Build(MakeScreenMap(109, 0, 360.0f, kVpos), kHpos, kVpos);
    copraster::Build(in, map, TestPalette(), QuietOptions(), out);

    ASSERT_EQ(out.width, 239);
    ASSERT_EQ(out.lineCC, 227);

    // Line 10's wrap column 227+3 mirrors line 11's clock 3 -- pixel and cell alike.
    const auto* wrapped = out.At(227 + 3, 10);
    ASSERT_NE(wrapped, nullptr);
    EXPECT_EQ(wrapped->dmaType, adm::AmigaEmulator::kDmaRecordBitplane);
    EXPECT_EQ(out.pixels[10 * out.width + 227 + 3], out.pixels[11 * out.width + 3]);
    EXPECT_EQ(out.At(227, 10)->dmaType, 0) << "column 227 is the next line's clock 0";

    // Counted once each, on the rows that own them.
    EXPECT_EQ(out.dmaCells, 2);

    // The last row has no next line to borrow from; its own clock 3 stays on the left.
    EXPECT_EQ(out.pixels[(out.height - 1) * out.width + 227 + 3], 0u);
    EXPECT_NE(out.pixels[(out.height - 1) * out.width + 3], 0u);
}

TEST(CopperRasterBuild, ALongNtscLineWrapsOneClockLater)
{
    // NTSC alternates 227- and 228-clock lines. PuAE marks a long one with LOL, and its extra
    // clock is real: the wrap for that row starts at 228.
    auto records = MakeRecords();
    SlotAt(records, 10, 5).evt = kEvtLol;
    SlotAt(records, 10, 227).type = adm::AmigaEmulator::kDmaRecordCpu;
    SlotAt(records, 11, 0).type = adm::AmigaEmulator::kDmaRecordBitplane;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;

    copraster::Result out;
    const auto map = RasterFrameMap::Build(MakeScreenMap(109, 0, 360.0f, kVpos), kHpos, kVpos);
    copraster::Build(in, map, TestPalette(), QuietOptions(), out);

    EXPECT_EQ(out.LineCCAt(10), 228);
    EXPECT_EQ(out.LineCCAt(11), 227);
    EXPECT_EQ(out.At(227, 10)->dmaType, adm::AmigaEmulator::kDmaRecordCpu);
    EXPECT_EQ(out.At(228, 10)->dmaType, adm::AmigaEmulator::kDmaRecordBitplane);
}

TEST(CopperRasterBuild, TheTextureAlwaysCoversTheWholeRaster)
{
    // The overlay stretches the texture over the raster rect the map describes. A record a line
    // shorter than the map must not produce a texture a line shorter, or every row creeps.
    auto records = MakeRecords(kHpos, 312);
    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = 312;

    copraster::Result out;
    const auto map = RasterFrameMap::Build(MakeScreenMap(109, 26, 360.0f, 287.0f), kHpos, 312);
    copraster::Build(in, map, TestPalette(), QuietOptions(), out);

    EXPECT_EQ(map.rasterLines, 313);
    EXPECT_EQ(out.height, map.rasterLines);
    EXPECT_EQ(out.width, map.rasterCC);
}

TEST(CopperRasterBuild, ChannelMaskHidesAChannelEntirely)
{
    auto records = MakeRecords();
    SlotAt(records, 5, 20).type = adm::AmigaEmulator::kDmaRecordBlitter;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;

    auto opt = QuietOptions();
    opt.channelMask = static_cast<uint16_t>(~(1u << adm::AmigaEmulator::kDmaRecordBlitter));

    copraster::Result out;
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), TestPalette(), opt,
                     out);

    EXPECT_EQ(out.dmaCells, 0);
    EXPECT_EQ(out.pixels[5 * out.width + 20], 0u);
    EXPECT_EQ(out.At(20, 5)->dmaSlot, -1);
}

// --- Build: copper layer --------------------------------------------------------------------

TEST(CopperRasterBuild, CopperInstructionsLandOnTheirSlotAndIndexBack)
{
    std::vector<CopEvent> events(2);
    events[0].vpos = 12;
    events[0].hpos = 30;
    events[0].word1 = 0x0180; // COLOR00
    events[0].word2 = 0x0F00;
    events[1].vpos = 12;
    events[1].hpos = 34;
    events[1].word1 = 0x0120; // SPR0PTH
    events[1].word2 = 0x0002;

    copraster::Input in;
    in.copperEvents = events.data();
    in.copperEventCount = 2;
    in.vposCount = kVpos;

    copraster::Result out;
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), TestPalette(),
                     QuietOptions(), out);

    EXPECT_EQ(out.copperCells, 2);

    const auto* a = out.At(30, 12);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->copperEvent, 0);
    EXPECT_EQ(a->copClass, CopClass::MoveColor);

    const auto* b = out.At(34, 12);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->copperEvent, 1);
    EXPECT_EQ(b->copClass, CopClass::MoveSprite);
}

TEST(CopperRasterBuild, CopperDrawsOverTheDmaSlotItOccupies)
{
    auto records = MakeRecords();
    SlotAt(records, 12, 30).type = adm::AmigaEmulator::kDmaRecordCopper;

    std::vector<CopEvent> events(1);
    events[0].vpos = 12;
    events[0].hpos = 30;
    events[0].word1 = 0x0180;
    events[0].word2 = 0x0F00;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;
    in.copperEvents = events.data();
    in.copperEventCount = 1;

    copraster::Result out;
    const auto pal = TestPalette();
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), pal, QuietOptions(),
                     out);

    // The cell knows about both, and the copper class colour is what shows.
    const auto* cell = out.At(30, 12);
    ASSERT_NE(cell, nullptr);
    EXPECT_EQ(cell->copperEvent, 0);
    EXPECT_EQ(cell->dmaType, adm::AmigaEmulator::kDmaRecordCopper);
    EXPECT_EQ(out.pixels[12 * out.width + 30],
              pal.copper[static_cast<size_t>(CopClass::MoveColor)]);
}

// --- Build: WAIT stall runs -----------------------------------------------------------------

TEST(CopperRasterBuild, WaitStallRunEndsAtTheCopperWakeEvent)
{
    auto records = MakeRecords();
    // The Copper resumes at clock 20 on the same line.
    SlotAt(records, 8, 20).evt = kEvtCopperWake;

    std::vector<CopEvent> events(2);
    events[0].vpos = 8;
    events[0].hpos = 10;
    events[0].word1 = 0x2C01; // WAIT
    events[0].word2 = 0xFF00;
    events[1].vpos = 8;
    events[1].hpos = 30; // the next instruction, well past the wake
    events[1].word1 = 0x0180;
    events[1].word2 = 0x0F00;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;
    in.copperEvents = events.data();
    in.copperEventCount = 2;

    auto opt = QuietOptions();
    opt.showWaitSpans = true;

    copraster::Result out;
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), TestPalette(), opt,
                     out);

    EXPECT_FALSE(out.At(10, 8)->waitBlocked) << "the WAIT slot itself is the instruction";
    EXPECT_TRUE(out.At(11, 8)->waitBlocked);
    EXPECT_TRUE(out.At(19, 8)->waitBlocked);
    EXPECT_TRUE(out.At(20, 8)->waitBlocked) << "the wake slot closes the run inclusively";
    EXPECT_FALSE(out.At(21, 8)->waitBlocked) << "stall must not run past the wake";
}

TEST(CopperRasterBuild, WaitStallRunIsBoundedByTheNextInstructionWithoutAWake)
{
    // No COPPERWAKE recorded at all: the run must still stop at the next copper event rather
    // than painting the rest of the frame red.
    auto records = MakeRecords();

    std::vector<CopEvent> events(2);
    events[0].vpos = 8;
    events[0].hpos = 10;
    events[0].word1 = 0x2C01;
    events[0].word2 = 0xFF00;
    events[1].vpos = 8;
    events[1].hpos = 15;
    events[1].word1 = 0x0180;
    events[1].word2 = 0x0F00;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;
    in.copperEvents = events.data();
    in.copperEventCount = 2;

    auto opt = QuietOptions();
    opt.showWaitSpans = true;

    copraster::Result out;
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), TestPalette(), opt,
                     out);

    EXPECT_TRUE(out.At(14, 8)->waitBlocked);
    EXPECT_TRUE(out.At(15, 8)->waitBlocked);
    EXPECT_FALSE(out.At(16, 8)->waitBlocked);
}

TEST(CopperRasterBuild, WaitStallRunCrossesAScanlineBoundary)
{
    auto records = MakeRecords();
    SlotAt(records, 9, 5).evt = kEvtCopperWake;

    std::vector<CopEvent> events(1);
    events[0].vpos = 8;
    events[0].hpos = 220;
    events[0].word1 = 0x2C01;
    events[0].word2 = 0xFF00;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;
    in.copperEvents = events.data();
    in.copperEventCount = 1;

    auto opt = QuietOptions();
    opt.showWaitSpans = true;

    copraster::Result out;
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), TestPalette(), opt,
                     out);

    EXPECT_TRUE(out.At(225, 8)->waitBlocked) << "tail of the starting line";
    EXPECT_TRUE(out.At(2, 9)->waitBlocked) << "continues onto the next line";
    EXPECT_TRUE(out.At(5, 9)->waitBlocked) << "wake slot";
    EXPECT_FALSE(out.At(6, 9)->waitBlocked);
}

// --- Build: event annotations ---------------------------------------------------------------

TEST(CopperRasterBuild, CopperWantedMarksABusDenial)
{
    auto records = MakeRecords();
    SlotAt(records, 4, 60).type = adm::AmigaEmulator::kDmaRecordBitplane;
    SlotAt(records, 4, 60).evt = kEvtCopperWanted;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;

    auto opt = QuietOptions();
    opt.showCopperWanted = true;

    copraster::Result out;
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), TestPalette(), opt,
                     out);

    EXPECT_TRUE(out.At(60, 4)->copperWanted);
    EXPECT_FALSE(out.At(61, 4)->copperWanted);
}

TEST(CopperRasterBuild, BlankingShadesTheWrappedRegionOfTheLine)
{
    // Blanking starts near the end of the line and ends early on the next — the wrap case that
    // a naive start < stop loop would either skip or invert.
    auto records = MakeRecords();
    SlotAt(records, 3, 200).evt = kEvtHbs;
    SlotAt(records, 3, 20).evt = kEvtHbe;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;

    auto opt = QuietOptions();
    opt.showBlanking = true;

    copraster::Result out;
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), TestPalette(), opt,
                     out);

    EXPECT_NE(out.pixels[3 * out.width + 210], 0u) << "inside the wrapped blank region";
    EXPECT_NE(out.pixels[3 * out.width + 5], 0u) << "wraps past the end of the line";
    EXPECT_EQ(out.pixels[3 * out.width + 100], 0u) << "the visible middle stays clear";
}

TEST(CopperRasterBuild, GeometryEventsAreMarkedPerLine)
{
    auto records = MakeRecords();
    SlotAt(records, 7, 56).evt = kEvtDdfStrt;
    SlotAt(records, 8, 60).evt = kEvtDdfStrt; // moved by the copper on the next line

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;

    auto opt = QuietOptions();
    opt.showGeometryEvents = true;

    copraster::Result out;
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), TestPalette(), opt,
                     out);

    EXPECT_NE(out.pixels[7 * out.width + 56], 0u);
    EXPECT_EQ(out.pixels[7 * out.width + 60], 0u);
    EXPECT_NE(out.pixels[8 * out.width + 60], 0u) << "the stepped edge is the point";
}

TEST(CopperRasterBuild, AnEventPastTheLineEndIsDrawnOnTheLineItEnds)
{
    // PuAE finishes a displayed line during the next Agnus line, so a display window closing at
    // beam clock 228 on line 10 is recorded on row 11 at hpos 228. It belongs at row 10, column
    // 228 -- measured: DIWSTOP moved off the end of the counter on one line removed the HDIWE
    // from the following row, not its own.
    constexpr int kWide = 288; // NR_DMA_REC_HPOS
    auto records = MakeRecords(kWide, kVpos);
    SlotAt(records, 11, 228, kWide).evt = kEvtHdiwe;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kWide;
    in.vposCount = kVpos;

    auto opt = QuietOptions();
    opt.showGeometryEvents = true;

    copraster::Result out;
    const auto map = RasterFrameMap::Build(MakeScreenMap(109, 0, 360.0f, kVpos), kWide, kVpos);
    copraster::Build(in, map, TestPalette(), opt, out);

    EXPECT_NE(out.pixels[10 * out.width + 228], 0u) << "on the line the window closed";
    EXPECT_EQ(out.pixels[11 * out.width + 228], 0u) << "not the row that recorded it";
}

// --- Frame height derivation ------------------------------------------------------------------
// Regression guard for a real bug: PuAE hands out a record buffer NR_DMA_REC_VPOS = 1000 rows
// tall, and taking that as the frame height stretched the raster to three times the real frame,
// squashing the emulator picture into a corner of it.

TEST(CopperRasterFrameHeight, DerivesRealHeightFromABufferMuchTallerThanTheFrame)
{
    constexpr int kBufferRows = 1000;
    auto records = MakeRecords(kHpos, kBufferRows);

    // Refresh DMA runs on every scanline of a real frame, including blanking.
    for (int v = 0; v < 313; ++v)
        SlotAt(records, v, 1).type = adm::AmigaEmulator::kDmaRecordRefresh;

    EXPECT_EQ(copraster::DeriveFrameHeight(records.data(), kHpos, kBufferRows, 999), 313);
}

TEST(CopperRasterFrameHeight, IgnoresTheRecordsPaddingColumns)
{
    // Activity past the real end of the line is buffer padding and must not extend the frame.
    constexpr int kWide = 288; // NR_DMA_REC_HPOS
    auto records = MakeRecords(kWide, 400);
    for (int v = 0; v < 200; ++v)
        SlotAt(records, v, 1, kWide).type = adm::AmigaEmulator::kDmaRecordRefresh;
    SlotAt(records, 350, 250, kWide).type = adm::AmigaEmulator::kDmaRecordBitplane;

    EXPECT_EQ(copraster::DeriveFrameHeight(records.data(), kWide, 400, 999), 200);
}

TEST(CopperRasterFrameHeight, CountsEventOnlyRowsAsPartOfTheFrame)
{
    // A blanking line may carry no DMA at all but still record HBS/HBE.
    auto records = MakeRecords(kHpos, 500);
    for (int v = 0; v < 260; ++v)
        SlotAt(records, v, 1).type = adm::AmigaEmulator::kDmaRecordRefresh;
    SlotAt(records, 262, 20).evt = kEvtHbs;

    EXPECT_EQ(copraster::DeriveFrameHeight(records.data(), kHpos, 500, 999), 263);
}

TEST(CopperRasterFrameHeight, FallsBackWhenNothingWasRecorded)
{
    auto records = MakeRecords(kHpos, 500);
    EXPECT_EQ(copraster::DeriveFrameHeight(records.data(), kHpos, 500, 313), 313);
    EXPECT_EQ(copraster::DeriveFrameHeight(nullptr, kHpos, 500, 313), 313);
    EXPECT_EQ(copraster::DeriveFrameHeight(records.data(), 0, 0, 313), 313);
}

TEST(RasterFrameMapTest, ClampsAnAbsurdLineCountSoTheImageIsNotSquashed)
{
    // Backstop for the same bug at the map layer: handed the raw buffer height, the map must not
    // produce a raster three times the height of the frame.
    const auto map = RasterFrameMap::Build(MakeScreenMap(), kHpos, 1000);
    ASSERT_TRUE(map.valid);
    EXPECT_LE(map.rasterLines, 320);
    // The picture must stay a sane fraction of the raster rather than collapsing into a sliver.
    EXPECT_LT(map.RasterToImageY(), 1.5f);
}

// --- Build: partial frame and robustness ------------------------------------------------------

TEST(CopperRasterBuild, BreakLRefDimsWhatTheBeamHadNotReached)
{
    auto records = MakeRecords();
    SlotAt(records, 2, 10).type = adm::AmigaEmulator::kDmaRecordBitplane;
    SlotAt(records, 20, 10).type = adm::AmigaEmulator::kDmaRecordBitplane;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;

    auto opt = QuietOptions();
    opt.breakLRef = 10u * 228u; // stopped at the start of line 10

    copraster::Result out;
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), TestPalette(), opt,
                     out);

    const uint32_t executed = out.pixels[2 * out.width + 10];
    const uint32_t notReached = out.pixels[20 * out.width + 10];
    EXPECT_EQ(copraster::AlphaOf(executed), 255u);
    EXPECT_LT(copraster::AlphaOf(notReached), 255u);
}

TEST(CopperRasterBuild, AnInvalidMapProducesAnEmptyResultRatherThanCrashing)
{
    copraster::Input in;
    copraster::Result out;
    copraster::Build(in, RasterFrameMap{}, TestPalette(), QuietOptions(), out);

    EXPECT_EQ(out.width, 0);
    EXPECT_EQ(out.height, 0);
    EXPECT_TRUE(out.pixels.empty());
    EXPECT_TRUE(out.cells.empty());
}

TEST(CopperRasterBuild, CopperEventsAloneWorkWithNoDmaRecord)
{
    // The DMA record is opt-in; the copper trace is not. With debug_dma off the overlay must
    // still draw the copper layer instead of coming out blank.
    std::vector<CopEvent> events(1);
    events[0].vpos = 3;
    events[0].hpos = 7;
    events[0].word1 = 0x0180;
    events[0].word2 = 0x0F00;

    copraster::Input in;
    in.copperEvents = events.data();
    in.copperEventCount = 1;
    in.vposCount = kVpos;

    copraster::Result out;
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), TestPalette(),
                     QuietOptions(), out);

    EXPECT_EQ(out.copperCells, 1);
    EXPECT_EQ(out.dmaCells, 0);
    EXPECT_EQ(out.At(7, 3)->copperEvent, 0);
}

TEST(CopperRasterBuild, OutOfRangeEventsAreIgnored)
{
    std::vector<CopEvent> events(2);
    events[0].vpos = static_cast<uint16_t>(kVpos + 5); // past the end of the frame
    events[0].hpos = 10;
    events[0].word1 = 0x0180;
    events[0].word2 = 0x0F00;
    events[1].vpos = 3;
    events[1].hpos = 250; // in the record's padding, past the real line
    events[1].word1 = 0x0180;
    events[1].word2 = 0x0F00;

    copraster::Input in;
    in.copperEvents = events.data();
    in.copperEventCount = 2;
    in.vposCount = kVpos;

    copraster::Result out;
    copraster::Build(in, RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos), TestPalette(),
                     QuietOptions(), out);

    EXPECT_EQ(out.copperCells, 0);
}

TEST(CopperRasterBuild, ReusingAResultClearsThePreviousFrame)
{
    auto records = MakeRecords();
    SlotAt(records, 6, 40).type = adm::AmigaEmulator::kDmaRecordSprite;

    copraster::Input in;
    in.records = records.data();
    in.hposCount = kHpos;
    in.vposCount = kVpos;

    copraster::Result out;
    const auto map = RasterFrameMap::Build(MakeScreenMap(), kHpos, kVpos);
    copraster::Build(in, map, TestPalette(), QuietOptions(), out);
    ASSERT_EQ(out.dmaCells, 1);

    // Second frame: the slot went away.
    SlotAt(records, 6, 40).type = 0;
    copraster::Build(in, map, TestPalette(), QuietOptions(), out);

    EXPECT_EQ(out.dmaCells, 0);
    EXPECT_EQ(out.pixels[6 * out.width + 40], 0u);
    EXPECT_EQ(out.At(40, 6)->dmaSlot, -1);
}
