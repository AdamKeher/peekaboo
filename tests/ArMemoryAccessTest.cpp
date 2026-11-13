#include <gtest/gtest.h>

#include <vector>

#include "../src/core/ArMemoryAccess.h"

using adm::core::ArMemoryAccess;

namespace {

// A memory view with only the CIA hook wired, standing in for a running machine.
ArMemoryAccess MakeCiaOnlyView(bool& sawChipB, int& sawReg)
{
    ArMemoryAccess mem;
    mem.readCiaReg = [&sawChipB, &sawReg](bool chipB, int reg, uint8_t& out) {
        sawChipB = chipB;
        sawReg = reg;
        // Distinguishable per chip so a mis-decode cannot pass by coincidence.
        out = static_cast<uint8_t>((chipB ? 0xB0 : 0xA0) | reg);
        return true;
    };
    return mem;
}

} // namespace

// $BFE001 is CIA-A PRA, where the joystick fire buttons live. read_memory used to
// return "??" for it, which reads as "the button did nothing".
TEST(ArMemoryAccessTest, DecodesCiaAOnOddAddresses)
{
    bool chipB = true;
    int reg = -1;
    ASSERT_TRUE(ArMemoryAccess::DecodeCia(0xBFE001, chipB, reg));
    EXPECT_FALSE(chipB);
    EXPECT_EQ(reg, 0x0); // PRA

    ASSERT_TRUE(ArMemoryAccess::DecodeCia(0xBFED01, chipB, reg));
    EXPECT_FALSE(chipB);
    EXPECT_EQ(reg, 0xD); // ICR
}

TEST(ArMemoryAccessTest, DecodesCiaBOnEvenAddresses)
{
    bool chipB = false;
    int reg = -1;
    ASSERT_TRUE(ArMemoryAccess::DecodeCia(0xBFD000, chipB, reg));
    EXPECT_TRUE(chipB);
    EXPECT_EQ(reg, 0x0);

    ASSERT_TRUE(ArMemoryAccess::DecodeCia(0xBFDF00, chipB, reg));
    EXPECT_TRUE(chipB);
    EXPECT_EQ(reg, 0xF); // CRB
}

// Wrong parity means no chip select asserts; inventing a value there would be worse
// than admitting the read did not land.
TEST(ArMemoryAccessTest, RejectsCiaAddressesNoChipAnswers)
{
    bool chipB = false;
    int reg = -1;
    EXPECT_FALSE(ArMemoryAccess::DecodeCia(0xBFE000, chipB, reg)); // CIA-A needs A0 set
    EXPECT_FALSE(ArMemoryAccess::DecodeCia(0xBFD001, chipB, reg)); // CIA-B needs A0 clear
    EXPECT_FALSE(ArMemoryAccess::DecodeCia(0x000400, chipB, reg)); // not CIA space at all
    EXPECT_FALSE(ArMemoryAccess::DecodeCia(0xDFF000, chipB, reg)); // custom chip space
}

TEST(ArMemoryAccessTest, ReadsCiaRegistersThroughTheHook)
{
    bool chipB = false;
    int reg = -1;
    ArMemoryAccess mem = MakeCiaOnlyView(chipB, reg);

    bool ok = false;
    EXPECT_EQ(mem.ReadByte(0xBFE001, &ok), 0xA0); // CIA-A PRA
    EXPECT_TRUE(ok);
    EXPECT_FALSE(chipB);

    EXPECT_EQ(mem.ReadByte(0xBFD100, &ok), 0xB1); // CIA-B PRB
    EXPECT_TRUE(ok);
    EXPECT_TRUE(chipB);

    // An address in CIA space that no chip decodes stays invalid rather than
    // borrowing a neighbouring register's value.
    mem.ReadByte(0xBFE000, &ok);
    EXPECT_FALSE(ok);
}

// Without the hook (no emulator running) CIA space must report unmapped, not zeroes.
TEST(ArMemoryAccessTest, CiaSpaceIsUnmappedWithoutAHook)
{
    ArMemoryAccess mem;
    bool ok = true;
    mem.ReadByte(0xBFE001, &ok);
    EXPECT_FALSE(ok);
}

// Region-backed RAM must still win over the CIA decode.
TEST(ArMemoryAccessTest, MappedRegionsTakePrecedence)
{
    std::vector<uint8_t> ram(4, 0x5A);
    ArMemoryAccess mem;
    mem.SetRegions({{ram.data(), 0xBFE000, 4, "fake", true}});
    mem.readCiaReg = [](bool, int, uint8_t& out) {
        out = 0xFF;
        return true;
    };
    bool ok = false;
    EXPECT_EQ(mem.ReadByte(0xBFE001, &ok), 0x5A);
    EXPECT_TRUE(ok);
}
