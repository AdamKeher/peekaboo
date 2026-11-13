#include "core/CpuExtState.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

using namespace adm::core;

// UAE's m2cregs[] register numbers, in the order the core reports movec[].
constexpr int kM2cRegno[18] = {0,     1,     2,     3,     4,     5,     6,     7,     8,
                               0x800, 0x801, 0x802, 0x803, 0x804, 0x805, 0x806, 0x807, 0x808};

// newcpu_common.c movec_illg(), transcribed, so the catalogue can be checked against the core.
bool MovecIllegal(int cpuModel, int regno)
{
    const int regno2 = regno & 0x7ff;
    switch (cpuModel) {
        case 68060:
            if (regno <= 8)
                return false;
            return !(regno == 0x800 || regno == 0x801 || regno == 0x806 || regno == 0x807 ||
                     regno == 0x808);
        case 68010:
            return regno2 >= 2;
        case 68020:
            if (regno == 3)
                return true;
            return !(regno2 < 4 || regno == 0x804);
        case 68030:
            if (regno2 <= 2)
                return false;
            return !(regno == 0x803 || regno == 0x804);
        case 68040:
            if (regno == 0x802)
                return true;
            return regno2 >= 8;
        default:
            return true;
    }
}

// What the core would hand back for this machine, values = the register's movec index + 1.
CpuExtendedState CoreState(int cpuModel, int fpuModel = 0, int mmuModel = 0)
{
    CpuExtendedState st;
    st.cpuModel = cpuModel;
    st.fpuModel = fpuModel;
    st.mmuModel = mmuModel;
    st.supervisor = true;
    st.usp = 0x1000;
    st.isp = 0x2000;
    st.msp = 0x3000;
    for (int i = 0; i < 18; ++i) {
        st.movec[i] = 0x100 + i;
        if (cpuModel > 68000 && !MovecIllegal(cpuModel, kM2cRegno[i]))
            st.movecValid |= 1u << i;
    }
    st.fpuPresent = fpuModel != 0;
    st.mmu030Present = cpuModel == 68030;
    st.tc030 = 0x80C07760;
    st.crp030 = 0x7FFF000200001000ull;
    st.mmusr030 = 0x0A01;
    return st;
}

TEST(CpuExtState, CatalogueAgreesWithTheCoresMovecTable)
{
    // A MOVEC register the catalogue says a CPU has must carry the core's validity bit, and the
    // reverse - otherwise the window would grey something live or show a stale shadow.
    for (int cpu : {68000, 68010, 68020, 68030, 68040, 68060}) {
        const auto st = CoreState(cpu, cpu >= 68020 ? 68882 : 0);
        for (const auto& def : CpuRegisterCatalog()) {
            if (def.group == CpuRegGroup::Fpu || def.id == CpuRegId::Usp || def.id == CpuRegId::Isp)
                continue;
            if (cpu == 68030 &&
                (def.id == CpuRegId::Tc || def.id == CpuRegId::Crp || def.id == CpuRegId::Srp ||
                 def.id == CpuRegId::Tt0 || def.id == CpuRegId::Tt1 || def.id == CpuRegId::Mmusr))
                continue; // PMOVE, not MOVEC
            EXPECT_EQ(CpuHasRegister(def, cpu, 0), ReadCpuRegister(st, def).has_value())
                << def.name << " on " << cpu;
        }
    }
}

TEST(CpuExtState, A68000HasOnlyItsTwoStacks)
{
    const auto st = CoreState(68000);
    int present = 0;
    for (const auto& def : CpuRegisterCatalog())
        if (ReadCpuRegister(st, def))
            ++present;
    EXPECT_EQ(present, 2);
    EXPECT_STREQ(IspName(68000), "SSP");
    EXPECT_EQ(ReadCpuRegister(st, GetCpuRegister(CpuRegId::Isp))->value, 0x2000u);
}

TEST(CpuExtState, The68030ReadsItsPmoveRegisters)
{
    const auto st = CoreState(68030, 68882, 68030);
    auto tc = ReadCpuRegister(st, GetCpuRegister(CpuRegId::Tc));
    ASSERT_TRUE(tc);
    EXPECT_EQ(tc->value, 0x80C07760u);
    auto crp = ReadCpuRegister(st, GetCpuRegister(CpuRegId::Crp));
    ASSERT_TRUE(crp);
    EXPECT_EQ(crp->hexDigits, 16);
    EXPECT_EQ(FormatCpuRegister(*crp), "7FFF0002:00001000");
    auto mmusr = ReadCpuRegister(st, GetCpuRegister(CpuRegId::Mmusr));
    ASSERT_TRUE(mmusr);
    EXPECT_EQ(FormatCpuRegister(*mmusr), "0A01");
    EXPECT_FALSE(ReadCpuRegister(st, GetCpuRegister(CpuRegId::Urp)));
    EXPECT_FALSE(ReadCpuRegister(st, GetCpuRegister(CpuRegId::Itt0)));
}

TEST(CpuExtState, The040TakesTcAndSrpFromMovec)
{
    const auto st = CoreState(68040, 68040, 68040);
    auto tc = ReadCpuRegister(st, GetCpuRegister(CpuRegId::Tc));
    ASSERT_TRUE(tc);
    EXPECT_EQ(tc->value, 0x103u);
    EXPECT_EQ(ReadCpuRegister(st, GetCpuRegister(CpuRegId::Srp))->hexDigits, 8);
    EXPECT_TRUE(ReadCpuRegister(st, GetCpuRegister(CpuRegId::Msp)));
    EXPECT_FALSE(ReadCpuRegister(st, GetCpuRegister(CpuRegId::Caar)));
}

TEST(CpuExtState, FpuRegistersNeedAnFpu)
{
    const auto& fp0 = GetCpuRegister(CpuRegId::Fp0);
    EXPECT_FALSE(CpuHasRegister(fp0, 68020, 0));
    EXPECT_TRUE(CpuHasRegister(fp0, 68020, 68881));
    EXPECT_FALSE(CpuHasRegister(fp0, 68000, 68881));
    EXPECT_EQ(NotOnCpuText(fp0, 68020), "FP0: no FPU fitted (68020+ with an FPU)");
    EXPECT_FALSE(ReadCpuRegister(CoreState(68020), GetCpuRegister(CpuRegId::Fpcr)));
    EXPECT_TRUE(ReadCpuRegister(CoreState(68020, 68881), GetCpuRegister(CpuRegId::Fpcr)));
}

TEST(CpuExtState, SupportedOnTextReadsNaturally)
{
    EXPECT_EQ(SupportedOnText(GetCpuRegister(CpuRegId::Vbr)), "68010+");
    EXPECT_EQ(SupportedOnText(GetCpuRegister(CpuRegId::Caar)), "68020 / 68030");
    EXPECT_EQ(SupportedOnText(GetCpuRegister(CpuRegId::Pcr)), "68060");
    EXPECT_EQ(SupportedOnText(GetCpuRegister(CpuRegId::Usp)), "every 680x0");
    EXPECT_EQ(NotOnCpuText(GetCpuRegister(CpuRegId::Vbr), 68000), "VBR: not on a 68000 (68010+)");
}

TEST(CpuExtState, FindsAliases)
{
    EXPECT_EQ(FindCpuRegister("ssp")->id, CpuRegId::Isp);
    EXPECT_EQ(FindCpuRegister("busc")->id, CpuRegId::Buscr);
    EXPECT_EQ(FindCpuRegister("Vbr")->id, CpuRegId::Vbr);
    EXPECT_EQ(FindCpuRegister("d0"), nullptr);
}

TEST(CpuExtState, SettingTheActiveStackMeansA7)
{
    auto st = CoreState(68030, 0, 68030);
    st.supervisor = true;
    st.master = false;
    EXPECT_STREQ(UaeSettableName(st, GetCpuRegister(CpuRegId::Isp)), "A7");
    EXPECT_STREQ(UaeSettableName(st, GetCpuRegister(CpuRegId::Usp)), "USP");
    EXPECT_STREQ(UaeSettableName(st, GetCpuRegister(CpuRegId::Vbr)), "VBR");
    EXPECT_EQ(UaeSettableName(st, GetCpuRegister(CpuRegId::Tc)), nullptr); // PMOVE on a 030
    EXPECT_EQ(UaeSettableName(CoreState(68000), GetCpuRegister(CpuRegId::Vbr)), nullptr);
    EXPECT_STREQ(UaeSettableName(CoreState(68060), GetCpuRegister(CpuRegId::Buscr)), "BUSC");
}

TEST(CpuExtState, DecodesFpcrAndFpsr)
{
    EXPECT_EQ(DecodeFpcr(0), "prec X round RN enable -");
    EXPECT_EQ(DecodeFpcr(0x00001090), "prec D round RZ enable OVFL");
    EXPECT_EQ(DecodeFpsr(0x08000208), "N=1 Z=0 I=0 NAN=0 Q=0 exc INEX2 acc INEX");
    EXPECT_EQ(DecodeFpsr(0x04850000), "N=0 Z=1 I=0 NAN=0 Q=-5 exc - acc -");
}

TEST(CpuExtState, ParsesTheCoreBlock)
{
    CpuExtRaw raw{};
    raw.structSize = sizeof(raw);
    raw.cpuModel = 68030;
    raw.fpuModel = 68882;
    raw.s = 1;
    raw.m = 1;
    raw.intmask = 3;
    raw.msp = 0x7F000;
    raw.fpuPresent = 1;
    raw.fpExt[0][0] = 0x40000000;
    raw.fpExt[0][1] = 0xC90FDAA2;
    raw.fpExt[0][2] = 0x2168C235;
    std::strcpy(raw.fpText[0], "#3.141592653589793e+00");
    raw.crpHi = 0x7FFF0002;
    raw.crpLo = 0x00001000;

    auto st = ParseCpuExtState(raw, sizeof(raw));
    ASSERT_TRUE(st);
    EXPECT_EQ(st->intmask, 3);
    EXPECT_STREQ(ActiveStackName(*st), "MSP");
    EXPECT_EQ(FormatFpRaw(st->fp[0]), "4000-C90FDAA2-2168C235");
    EXPECT_EQ(FormatFpValue(st->fp[0]), "3.141592653589793e+00");
    EXPECT_EQ(st->crp030, 0x7FFF000200001000ull);

    EXPECT_FALSE(ParseCpuExtState(raw, 16)); // an older, shorter layout
    raw.structSize = 12;
    EXPECT_FALSE(ParseCpuExtState(raw, sizeof(raw)));
}

TEST(CpuExtState, NamesTheFpuAndMmu)
{
    EXPECT_EQ(FpuName(0, 68020), "None");
    EXPECT_EQ(FpuName(68882, 68030), "68882");
    EXPECT_EQ(FpuName(68040, 68040), "68040 internal");
    auto st = CoreState(68030, 0, 68030);
    st.mmuEc = true;
    EXPECT_EQ(CpuMmuName(st), "68EC030");
}

} // namespace
