#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <vector>

#include "core/LibraryCallAnnotator.h"

using adm::core::LibraryCallAnnotator;

// ---------------------------------------------------------------------------
// ParseLibCall — recognise the d16(An) shape of a library call.
// ---------------------------------------------------------------------------

TEST(LibraryCallAnnotator, ParsesUaeCommaForm)
{
    int reg = -1;
    uint32_t lvo = 0;
    ASSERT_TRUE(LibraryCallAnnotator::ParseLibCall("JSR.L (a6,-$01c8) == $00c000ae", reg, lvo));
    EXPECT_EQ(reg, 6);
    EXPECT_EQ(lvo, 456u); // -$1c8 -> DoIO
}

TEST(LibraryCallAnnotator, ParsesPrefixDispForm)
{
    int reg = -1;
    uint32_t lvo = 0;
    ASSERT_TRUE(LibraryCallAnnotator::ParseLibCall("JSR -$0228(a6)", reg, lvo));
    EXPECT_EQ(reg, 6);
    EXPECT_EQ(lvo, 552u); // OpenLibrary
}

TEST(LibraryCallAnnotator, ParsesDecimalDisplacement)
{
    int reg = -1;
    uint32_t lvo = 0;
    ASSERT_TRUE(LibraryCallAnnotator::ParseLibCall("JMP (a5,-198)", reg, lvo));
    EXPECT_EQ(reg, 5);
    EXPECT_EQ(lvo, 198u);
}

TEST(LibraryCallAnnotator, RejectsNonCallsAndPositiveDisp)
{
    int reg = 0;
    uint32_t lvo = 0;
    EXPECT_FALSE(LibraryCallAnnotator::ParseLibCall("MOVE.L (a6,-$01c8),d0", reg, lvo));
    EXPECT_FALSE(LibraryCallAnnotator::ParseLibCall("JSR (a6,$0010)", reg, lvo)); // positive
    EXPECT_FALSE(LibraryCallAnnotator::ParseLibCall("RTS", reg, lvo));
    EXPECT_FALSE(LibraryCallAnnotator::ParseLibCall("JSR $00fc00d2", reg, lvo)); // absolute
}

// ---------------------------------------------------------------------------
// Annotate — table lookup with strategy (a) default and (c) resolution.
// ---------------------------------------------------------------------------

TEST(LibraryCallAnnotator, AnnotatesExecByDefault)
{
    LibraryCallAnnotator ann;
    ASSERT_TRUE(ann.LoadTables("assets/amiga_lvos.json"));

    auto s = ann.Annotate("JSR.L (a6,-$01c8)");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "  ; exec/DoIO()");

    EXPECT_EQ(ann.Annotate("JSR (a6,-$0228)").value_or(""), "  ; exec/OpenLibrary()");
}

TEST(LibraryCallAnnotator, ResolvesGraphicsViaLiveBaseAndMemory)
{
    LibraryCallAnnotator ann;
    ASSERT_TRUE(ann.LoadTables("assets/amiga_lvos.json"));

    // a6 -> graphics.library node. -$e4 == -228 is graphics/WaitBlit.
    constexpr uint32_t kBase = 0x5000;
    constexpr uint32_t kNamePtr = 0x6000;
    std::map<uint32_t, uint8_t> mem;
    for (int i = 0; i < 4; ++i)
        mem[kBase + 10 + i] = uint8_t(kNamePtr >> (24 - 8 * i));
    const char* name = "graphics.library";
    for (uint32_t i = 0; name[i]; ++i)
        mem[kNamePtr + i] = uint8_t(name[i]);
    mem[kNamePtr + 16] = 0;

    LibraryCallAnnotator::Context ctx;
    ctx.readAReg = [](int, uint32_t& val) {
        val = kBase;
        return true;
    };
    ctx.readByte = [&](uint32_t addr, uint8_t& out) {
        auto it = mem.find(addr);
        if (it == mem.end())
            return false;
        out = it->second;
        return true;
    };

    EXPECT_EQ(ann.Annotate("JSR (a6,-$e4)", ctx).value_or(""), "  ; graphics/WaitBlit()");
}

TEST(LibraryCallAnnotator, ReturnsNulloptForUnknownOffset)
{
    LibraryCallAnnotator ann;
    ASSERT_TRUE(ann.LoadTables("assets/amiga_lvos.json"));
    // -4 is not a real LVO; nothing to say.
    EXPECT_FALSE(ann.Annotate("JSR (a6,-$0004)").has_value());
}

TEST(LibraryCallAnnotator, ResolvesDosViaLiveBaseAndMemory)
{
    LibraryCallAnnotator ann;
    ASSERT_TRUE(ann.LoadTables("assets/amiga_lvos.json"));

    // Lay out a fake "dos.library" node: base at 0x1000, ln_Name (offset 10) ->
    // 0x2000 -> "dos.library\0".
    constexpr uint32_t kBase = 0x1000;
    constexpr uint32_t kNamePtr = 0x2000;
    std::map<uint32_t, uint8_t> mem;
    auto putLong = [&](uint32_t a, uint32_t v) {
        mem[a] = uint8_t(v >> 24);
        mem[a + 1] = uint8_t(v >> 16);
        mem[a + 2] = uint8_t(v >> 8);
        mem[a + 3] = uint8_t(v);
    };
    putLong(kBase + 10, kNamePtr);
    const char* name = "dos.library";
    for (uint32_t i = 0; name[i]; ++i)
        mem[kNamePtr + i] = uint8_t(name[i]);
    mem[kNamePtr + 11] = 0;

    LibraryCallAnnotator::Context ctx;
    ctx.readAReg = [](int aReg, uint32_t& val) {
        if (aReg != 6)
            return false;
        val = kBase;
        return true;
    };
    ctx.readByte = [&](uint32_t addr, uint8_t& out) {
        auto it = mem.find(addr);
        if (it == mem.end())
            return false;
        out = it->second;
        return true;
    };

    // -$1e == -30 is dos/Open; the resolved name picks the dos table, not exec.
    EXPECT_EQ(ann.Annotate("JSR (a6,-$1e)", ctx).value_or(""), "  ; dos/Open()");
}

TEST(LibraryCallAnnotator, SuppressesWhenResolvedLibraryHasNoTable)
{
    LibraryCallAnnotator ann;
    ASSERT_TRUE(ann.LoadTables("assets/amiga_lvos.json"));

    // a6 points at a library whose name we can read but have no table for. We must
    // NOT mislabel it as exec — annotation is suppressed.
    constexpr uint32_t kBase = 0x3000;
    constexpr uint32_t kNamePtr = 0x4000;
    std::map<uint32_t, uint8_t> mem;
    auto putLong = [&](uint32_t a, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            mem[a + i] = uint8_t(v >> (24 - 8 * i));
    };
    putLong(kBase + 10, kNamePtr);
    const char* name = "mathffp.library";
    for (uint32_t i = 0; name[i]; ++i)
        mem[kNamePtr + i] = uint8_t(name[i]);
    mem[kNamePtr + 15] = 0;

    LibraryCallAnnotator::Context ctx;
    ctx.readAReg = [](int, uint32_t& val) {
        val = kBase;
        return true;
    };
    ctx.readByte = [&](uint32_t addr, uint8_t& out) {
        auto it = mem.find(addr);
        if (it == mem.end())
            return false;
        out = it->second;
        return true;
    };

    EXPECT_FALSE(ann.Annotate("JSR (a6,-$01c8)", ctx).has_value());
}
