#include "core/AmigaCustom.h"
#include "core/AmigaEmulator.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

// The todo item this settles: "end_frame then screenshot still reported partialFrame: true at
// beam vpos 1".
//
// The screenshot and get_status tools called a capture partial when the beam was anywhere but
// vpos 0. A completed frame does not leave the beam at vpos 0 - retro_run returns a few lines
// into the next field's vertical blank - so every paused screenshot was labelled partial, and the
// advice to "call end_frame and screenshot again" could never clear it, because end_frame lands
// in the same place.
//
// What actually matters is whether the beam has descended into the display window of the new
// field: the framebuffer holds the previous field below the beam, so above DIWSTRT's vstart
// nothing visible has been overwritten and the buffer holds a whole field.
namespace {

#ifdef _WIN32
constexpr const char* kCoreName = "puae_libretro.dll";
#else
constexpr const char* kCoreName = "puae_libretro.dylib";
#endif

std::unique_ptr<adm::AmigaEmulator> BootA500()
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / kCoreName;
    if (!std::filesystem::is_regular_file(corePath))
        return nullptr;
    auto emulator = std::make_unique<adm::AmigaEmulator>();
    // Muted: StartEmpty() opens a real host playback device. See EmulatorWarmResetTest.
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    if (!emulator->SelectModelPreset(adm::AmigaModel::A500))
        return nullptr;
    if (!emulator->LoadCore(corePath.string()) || !emulator->StartEmpty())
        return nullptr;
    for (int i = 0; i < 100; ++i)
        emulator->StepOneFrame(false);
    return emulator;
}

// The measurement the old test got wrong: a whole frame does not end on vpos 0.
TEST(EndFrame, ACompletedFrameDoesNotLeaveTheBeamAtVposZero)
{
    auto emulator = BootA500();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core or ROM is not present in emulation/";

    bool sawNonZero = false;
    for (int i = 0; i < 8; ++i) {
        emulator->StepOneFrame(false);
        ASSERT_FALSE(emulator->IsPartialFrame()) << "a plain frame step should cross a boundary";
        int vpos = 0, hpos = 0;
        if (emulator->GetBeamPosition(vpos, hpos) && vpos != 0)
            sawNonZero = true;
    }
    EXPECT_TRUE(sawNonZero) << "if whole frames really do end on vpos 0, the old partialFrame "
                               "test was right and this fix is unnecessary";
}

// end_frame's own path: RunToFrameEnd leaves a complete frame, and the beam still is not at 0.
TEST(EndFrame, RunToFrameEndCompletesTheFrameButNotAtVposZero)
{
    auto emulator = BootA500();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core or ROM is not present in emulation/";

    emulator->RunToFrameEnd();
    EXPECT_FALSE(emulator->IsPartialFrame()) << "end_frame should leave a whole frame";

    int vpos = 0, hpos = 0;
    ASSERT_TRUE(emulator->GetBeamPosition(vpos, hpos));
    EXPECT_NE(vpos, 0) << "the old partialFrame test would have been right after all";

    // The whole point: the beam is parked in the vertical blank of the next field, above the
    // display window, so nothing visible has been overwritten and the capture is not partial.
    adm::core::Custom custom{};
    ASSERT_TRUE(emulator->GetCustomRegisters(custom));
    EXPECT_FALSE(adm::core::BeamHasOverwrittenVisibleField(custom, vpos))
        << "beam at vpos " << vpos << " judged inside the display window (first line "
        << adm::core::DisplayWindowFirstLine(custom) << ")";
}

// The predicate itself, away from any machine: what it does at the boundaries, and that a
// write-shadow DIWSTRT of 0 falls back to PAL's $2C rather than calling every line visible.
TEST(EndFrame, VisibleFieldPredicateUsesTheDisplayWindow)
{
    adm::core::Custom custom{};
    custom.diwstrt = 0x2C81; // vstart $2C, the usual PAL full-screen value
    EXPECT_EQ(adm::core::DisplayWindowFirstLine(custom), 0x2C);
    EXPECT_FALSE(adm::core::BeamHasOverwrittenVisibleField(custom, 0));
    EXPECT_FALSE(adm::core::BeamHasOverwrittenVisibleField(custom, 3));
    EXPECT_FALSE(adm::core::BeamHasOverwrittenVisibleField(custom, 0x2C));
    EXPECT_TRUE(adm::core::BeamHasOverwrittenVisibleField(custom, 0x2D));
    EXPECT_TRUE(adm::core::BeamHasOverwrittenVisibleField(custom, 200));

    adm::core::Custom unwritten{}; // diwstrt still 0: the shadow has not seen a write
    EXPECT_EQ(adm::core::DisplayWindowFirstLine(unwritten), adm::core::kDefaultDiwVStart);
    EXPECT_FALSE(adm::core::BeamHasOverwrittenVisibleField(unwritten, 3));
    EXPECT_TRUE(adm::core::BeamHasOverwrittenVisibleField(unwritten, 100));

    adm::core::Custom absurd{};
    absurd.diwstrt = 0xF000; // vstart $F0, below any field a display could follow
    EXPECT_EQ(adm::core::DisplayWindowFirstLine(absurd), adm::core::kDefaultDiwVStart);
}

} // namespace
