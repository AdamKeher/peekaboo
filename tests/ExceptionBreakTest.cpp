#include "core/AmigaEmulator.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

// The todo item this settles: "set_exception_break armed while a program is already running never
// stops". Armed before the machine ran it stopped on every vector tried; armed once the program
// was up - and again as pause, EB, resume - an ILLEGAL ran straight into the guest's own handler.
//
// Reproducing it needs an exception that happens on demand rather than only during Kickstart's
// boot-time CPU probe. Chip RAM is host-writable through MemoryRegion::hostPtr, so the test points
// the level-3 autovector at a longword of its own holding ILLEGAL ($4AFC): the next VBlank
// interrupt enters through vector 27 (masked out of every exception-break mask by the core, so it
// cannot itself stop us) and the handler's first instruction raises vector 4.
namespace {

#ifdef _WIN32
constexpr const char* kCoreName = "puae_libretro.dll";
#else
constexpr const char* kCoreName = "puae_libretro.dylib";
#endif

constexpr uint32_t kIllegalStub = 0x00070000; // Chip RAM, above anything KS 1.3 boots into
constexpr uint32_t kVector4Stub = 0x00070010;
constexpr int kLevel3Autovector = 0x6C; // VBlank enters here every frame
constexpr int kVector4 = 0x10;          // ILLEGAL instruction

void PokeWord(uint8_t* chip, uint32_t addr, uint16_t value)
{
    chip[addr] = static_cast<uint8_t>(value >> 8);
    chip[addr + 1] = static_cast<uint8_t>(value);
}

void PokeLong(uint8_t* chip, uint32_t addr, uint32_t value)
{
    PokeWord(chip, addr, static_cast<uint16_t>(value >> 16));
    PokeWord(chip, addr + 2, static_cast<uint16_t>(value));
}

// Makes the machine raise vector 4 once per frame, and survive doing it.
//
// The level-3 autovector is pointed at a stub whose first instruction is ILLEGAL, so every VBlank
// raises the exception. Vector 4 itself is pointed at a handler that steps the stacked PC past the
// ILLEGAL and returns, instead of letting Kickstart guru - so the guest stays alive and the next
// frame raises another one. Without that the machine is dead after the first hit and a test for
// the second hit would be measuring the corpse rather than the break.
//
// Entering the level-3 interrupt is vector 27, which the core masks out of every exception-break
// mask, so the interrupt itself cannot stop us.
//
// Returns false when Exec has not installed its vectors yet, so a caller can wait rather than
// assert on a machine that was not ready.
bool ArmIllegalEveryFrame(const adm::AmigaEmulator& emulator)
{
    for (const auto& region : emulator.GetMemoryRegions()) {
        if (region.name != "Chip RAM" || !region.hostPtr || region.amigaAddr != 0)
            continue;
        if (region.size <= kVector4Stub + 8)
            return false;
        uint8_t* chip = region.hostPtr;
        // The vector Exec installed must already point somewhere, or interrupts are not running
        // yet and nothing will ever reach the stub.
        const uint32_t installed = (static_cast<uint32_t>(chip[kLevel3Autovector]) << 24) |
                                   (static_cast<uint32_t>(chip[kLevel3Autovector + 1]) << 16) |
                                   (static_cast<uint32_t>(chip[kLevel3Autovector + 2]) << 8) |
                                   static_cast<uint32_t>(chip[kLevel3Autovector + 3]);
        if (installed == 0)
            return false;

        PokeWord(chip, kIllegalStub, 0x4AFC);     // illegal
        PokeWord(chip, kIllegalStub + 2, 0x4E73); // rte  (returns from the level-3 interrupt)

        PokeWord(chip, kVector4Stub, 0x54AF);     // addq.l #2,2(sp)  - step the stacked PC
        PokeWord(chip, kVector4Stub + 2, 0x0002); //                    past the ILLEGAL
        PokeWord(chip, kVector4Stub + 4, 0x4E73); // rte

        PokeLong(chip, kLevel3Autovector, kIllegalStub);
        PokeLong(chip, kVector4, kVector4Stub);
        return true;
    }
    return false;
}

// Steps up to `frames` frames, stopping at the first break. Returns the vector, or -1.
int RunUntilExceptionBreak(adm::AmigaEmulator& emulator, int frames)
{
    for (int i = 0; i < frames; ++i) {
        emulator.StepOneFrame(false);
        if (!emulator.IsDebugBreakPending())
            continue;
        int vector = -1;
        if (emulator.ConsumeExceptionHit(vector))
            return vector;
        return -2; // a break, but not an exception one
    }
    return -1;
}

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
    return emulator;
}

// The control: armed before the machine has run a frame, which is the case the report says works.
TEST(ExceptionBreak, ArmedBeforeTheMachineRunsStopsOnTheVector)
{
    auto emulator = BootA500();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core or ROM is not present in emulation/";
    if (!emulator->HasExceptionBreak())
        GTEST_SKIP() << "the bundled core does not export adm_arm_exception";

    // Armed before a single frame has run. The stub still goes in after the machine has settled,
    // exactly as in the other two tests - Exec rewrites the vector table during its own init, so
    // a stub written at frame 0 never survives to be executed. The only difference between these
    // three tests is when the mask was armed.
    emulator->ArmExceptionBreak(1ull << 4); // ILLEGAL
    for (int i = 0; i < 150; ++i)
        emulator->StepOneFrame(false);
    ASSERT_TRUE(ArmIllegalEveryFrame(*emulator)) << "Exec never installed a level-3 handler";

    EXPECT_EQ(RunUntilExceptionBreak(*emulator, 30), 4);
}

// The item under test: the mask is armed only after the machine has been running for a while.
TEST(ExceptionBreak, ArmedWhileTheMachineIsAlreadyRunningStillStops)
{
    auto emulator = BootA500();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core or ROM is not present in emulation/";
    if (!emulator->HasExceptionBreak())
        GTEST_SKIP() << "the bundled core does not export adm_arm_exception";

    for (int i = 0; i < 150; ++i)
        emulator->StepOneFrame(false);
    ASSERT_FALSE(emulator->IsDebugBreakPending()) << "the machine broke before anything was armed";

    emulator->ArmExceptionBreak(1ull << 4);
    ASSERT_TRUE(ArmIllegalEveryFrame(*emulator)) << "Exec never installed a level-3 handler";

    EXPECT_EQ(RunUntilExceptionBreak(*emulator, 30), 4);
}

// The other half of the report: paused, armed, resumed. Pausing is a host-side state, so at this
// level it is the same call sequence with the arm between two runs of frames - what differs is
// that a resume re-arms the breakpoint trace, which must not drop the exception mask.
TEST(ExceptionBreak, ArmedWhilePausedSurvivesTheResumeReArm)
{
    auto emulator = BootA500();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core or ROM is not present in emulation/";
    if (!emulator->HasExceptionBreak())
        GTEST_SKIP() << "the bundled core does not export adm_arm_exception";

    for (int i = 0; i < 150; ++i)
        emulator->StepOneFrame(false);
    emulator->ArmExceptionBreak(1ull << 4);
    // What EmulatorView does on every resume.
    if (emulator->HasArmDebug())
        emulator->ArmDebugTrace();
    ASSERT_TRUE(ArmIllegalEveryFrame(*emulator)) << "Exec never installed a level-3 handler";

    EXPECT_EQ(RunUntilExceptionBreak(*emulator, 30), 4);
}

// debug_exception() sets the ADM break flags and then falls through into activate_debugger(),
// which latches debugger_active = 1 and early-returns on every later call, skipping the
// set_special(SPCFLAG_BRK) it would otherwise issue. If the break depended on that, only the
// first one would ever be delivered - and the reported session had PC breakpoints that would
// have latched debugger_active long before the exception mask was armed. It does not: the
// second hit stops as well.
//
// ArmIllegalEveryFrame leaves the guest alive and raising one per frame, so these are genuine
// repeat hits rather than a machine that gurued after the first.
TEST(ExceptionBreak, KeepsStoppingOnEveryHitNotJustTheFirst)
{
    auto emulator = BootA500();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core or ROM is not present in emulation/";
    if (!emulator->HasExceptionBreak())
        GTEST_SKIP() << "the bundled core does not export adm_arm_exception";

    for (int i = 0; i < 150; ++i)
        emulator->StepOneFrame(false);
    emulator->ArmExceptionBreak(1ull << 4);

    ASSERT_TRUE(ArmIllegalEveryFrame(*emulator)) << "Exec never installed a level-3 handler";
    for (int hit = 1; hit <= 4; ++hit) {
        EXPECT_EQ(RunUntilExceptionBreak(*emulator, 30), 4) << "exception break " << hit;
        // What a resume does, so the next iteration is a genuine resume-and-break-again.
        if (emulator->HasArmDebug())
            emulator->ArmDebugTrace();
    }
}

} // namespace
