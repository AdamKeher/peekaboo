#include "CpuExtState.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace adm::core {
namespace {

// Index into CpuExtRaw::movec (UAE's m2cregs[] order); -1 for a register MOVEC can't reach.
constexpr int kMovecSfc = 0, kMovecDfc = 1, kMovecCacr = 2, kMovecTc = 3, kMovecItt0 = 4,
              kMovecItt1 = 5, kMovecDtt0 = 6, kMovecDtt1 = 7, kMovecBuscr = 8, kMovecVbr = 10,
              kMovecCaar = 11, kMovecMsp = 12, kMovecMmusr = 14, kMovecUrp = 15, kMovecSrp = 16,
              kMovecPcr = 17;

constexpr uint8_t k010Up = kCpu68010 | kCpu68020 | kCpu68030 | kCpu68040 | kCpu68060;
constexpr uint8_t k020Up = kCpu68020 | kCpu68030 | kCpu68040 | kCpu68060;
constexpr uint8_t k040Up = kCpu68040 | kCpu68060;

constexpr std::array<CpuRegDef, 32> kCatalog{{
    {CpuRegId::Usp, "USP", CpuRegGroup::Stack, kCpuAll, true, "User stack pointer"},
    {CpuRegId::Isp, "ISP", CpuRegGroup::Stack, kCpuAll, true,
     "Interrupt (supervisor) stack pointer; the only supervisor stack before the 68020"},
    {CpuRegId::Msp, "MSP", CpuRegGroup::Stack, kCpu68020 | kCpu68030 | kCpu68040, true,
     "Master stack pointer, in use while SR.M is set"},

    {CpuRegId::Vbr, "VBR", CpuRegGroup::System, k010Up, true, "Vector base register"},
    {CpuRegId::Sfc, "SFC", CpuRegGroup::System, k010Up, false, "Source function code"},
    {CpuRegId::Dfc, "DFC", CpuRegGroup::System, k010Up, false, "Destination function code"},
    {CpuRegId::Cacr, "CACR", CpuRegGroup::System, k020Up, false, "Cache control register"},
    {CpuRegId::Caar, "CAAR", CpuRegGroup::System, kCpu68020 | kCpu68030, true,
     "Cache address register"},
    {CpuRegId::Buscr, "BUSCR", CpuRegGroup::System, kCpu68060, false, "Bus control register"},
    {CpuRegId::Pcr, "PCR", CpuRegGroup::System, kCpu68060, false,
     "Processor configuration register"},

    {CpuRegId::Tc, "TC", CpuRegGroup::Mmu, kCpu68030 | k040Up, false, "Translation control"},
    {CpuRegId::Crp, "CRP", CpuRegGroup::Mmu, kCpu68030, true, "CPU root pointer (64-bit)"},
    {CpuRegId::Srp, "SRP", CpuRegGroup::Mmu, kCpu68030 | k040Up, true,
     "Supervisor root pointer (64-bit on the 68030)"},
    {CpuRegId::Urp, "URP", CpuRegGroup::Mmu, k040Up, true, "User root pointer"},
    {CpuRegId::Tt0, "TT0", CpuRegGroup::Mmu, kCpu68030, false, "Transparent translation 0"},
    {CpuRegId::Tt1, "TT1", CpuRegGroup::Mmu, kCpu68030, false, "Transparent translation 1"},
    {CpuRegId::Itt0, "ITT0", CpuRegGroup::Mmu, k040Up, false,
     "Instruction transparent translation 0"},
    {CpuRegId::Itt1, "ITT1", CpuRegGroup::Mmu, k040Up, false,
     "Instruction transparent translation 1"},
    {CpuRegId::Dtt0, "DTT0", CpuRegGroup::Mmu, k040Up, false, "Data transparent translation 0"},
    {CpuRegId::Dtt1, "DTT1", CpuRegGroup::Mmu, k040Up, false, "Data transparent translation 1"},
    {CpuRegId::Mmusr, "MMUSR", CpuRegGroup::Mmu, kCpu68030 | kCpu68040, false, "MMU status"},

    {CpuRegId::Fp0, "FP0", CpuRegGroup::Fpu, k020Up, false, "Floating-point data register"},
    {CpuRegId::Fp1, "FP1", CpuRegGroup::Fpu, k020Up, false, "Floating-point data register"},
    {CpuRegId::Fp2, "FP2", CpuRegGroup::Fpu, k020Up, false, "Floating-point data register"},
    {CpuRegId::Fp3, "FP3", CpuRegGroup::Fpu, k020Up, false, "Floating-point data register"},
    {CpuRegId::Fp4, "FP4", CpuRegGroup::Fpu, k020Up, false, "Floating-point data register"},
    {CpuRegId::Fp5, "FP5", CpuRegGroup::Fpu, k020Up, false, "Floating-point data register"},
    {CpuRegId::Fp6, "FP6", CpuRegGroup::Fpu, k020Up, false, "Floating-point data register"},
    {CpuRegId::Fp7, "FP7", CpuRegGroup::Fpu, k020Up, false, "Floating-point data register"},
    {CpuRegId::Fpcr, "FPCR", CpuRegGroup::Fpu, k020Up, false, "FPU control register"},
    {CpuRegId::Fpsr, "FPSR", CpuRegGroup::Fpu, k020Up, false, "FPU status register"},
    {CpuRegId::Fpiar, "FPIAR", CpuRegGroup::Fpu, k020Up, true, "FPU instruction address register"},
}};

// GetCpuRegister() indexes the catalogue by CpuRegId.
constexpr bool CatalogInIdOrder()
{
    for (size_t i = 0; i < kCatalog.size(); ++i)
        if (static_cast<size_t>(kCatalog[i].id) != i)
            return false;
    return true;
}
static_assert(CatalogInIdOrder());

int MovecIndex(CpuRegId id)
{
    switch (id) {
        case CpuRegId::Vbr:
            return kMovecVbr;
        case CpuRegId::Sfc:
            return kMovecSfc;
        case CpuRegId::Dfc:
            return kMovecDfc;
        case CpuRegId::Cacr:
            return kMovecCacr;
        case CpuRegId::Caar:
            return kMovecCaar;
        case CpuRegId::Buscr:
            return kMovecBuscr;
        case CpuRegId::Pcr:
            return kMovecPcr;
        case CpuRegId::Msp:
            return kMovecMsp;
        case CpuRegId::Tc:
            return kMovecTc;
        case CpuRegId::Srp:
            return kMovecSrp;
        case CpuRegId::Urp:
            return kMovecUrp;
        case CpuRegId::Itt0:
            return kMovecItt0;
        case CpuRegId::Itt1:
            return kMovecItt1;
        case CpuRegId::Dtt0:
            return kMovecDtt0;
        case CpuRegId::Dtt1:
            return kMovecDtt1;
        case CpuRegId::Mmusr:
            return kMovecMmusr;
        default:
            return -1;
    }
}

bool IsFpRegister(CpuRegId id) { return id >= CpuRegId::Fp0 && id <= CpuRegId::Fp7; }

// The 68030 reaches these with PMOVE, not MOVEC; the core reports them separately.
bool Is030Pmove(CpuRegId id, int cpuModel)
{
    if (cpuModel != 68030)
        return false;
    return id == CpuRegId::Tc || id == CpuRegId::Crp || id == CpuRegId::Srp ||
           id == CpuRegId::Tt0 || id == CpuRegId::Tt1 || id == CpuRegId::Mmusr;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) ==
                      std::toupper(static_cast<unsigned char>(y));
           });
}

} // namespace

std::optional<CpuExtendedState> ParseCpuExtState(const CpuExtRaw& raw, int returned)
{
    if (returned < static_cast<int>(sizeof(CpuExtRaw)) || raw.structSize != sizeof(CpuExtRaw))
        return std::nullopt;
    CpuExtendedState st;
    st.cpuModel = static_cast<int>(raw.cpuModel);
    st.fpuModel = static_cast<int>(raw.fpuModel);
    st.mmuModel = static_cast<int>(raw.mmuModel);
    st.mmuEc = raw.mmuEc != 0;
    st.supervisor = raw.s != 0;
    st.master = raw.m != 0;
    st.intmask = static_cast<int>(raw.intmask & 7);
    st.stopped = raw.stopped != 0;
    st.usp = raw.usp;
    st.isp = raw.isp;
    st.msp = raw.msp;
    st.movecValid = raw.movecValid;
    std::copy(std::begin(raw.movec), std::end(raw.movec), st.movec.begin());
    st.fpuPresent = raw.fpuPresent != 0;
    for (int i = 0; i < 8; ++i) {
        std::copy(std::begin(raw.fpExt[i]), std::end(raw.fpExt[i]), st.fp[i].ext);
        const char* text = raw.fpText[i];
        st.fp[i].text.assign(text, strnlen(text, sizeof(raw.fpText[i])));
    }
    st.fpcr = raw.fpcr;
    st.fpsr = raw.fpsr;
    st.fpiar = raw.fpiar;
    st.mmu030Present = raw.mmu030Present != 0;
    st.crp030 = (uint64_t(raw.crpHi) << 32) | raw.crpLo;
    st.srp030 = (uint64_t(raw.srpHi) << 32) | raw.srpLo;
    st.tc030 = raw.tc030;
    st.tt0030 = raw.tt0030;
    st.tt1030 = raw.tt1030;
    st.mmusr030 = static_cast<uint16_t>(raw.mmusr030);
    return st;
}

uint8_t CpuModelBit(int cpuModel)
{
    switch (cpuModel) {
        case 68000:
            return kCpu68000;
        case 68010:
            return kCpu68010;
        case 68020:
            return kCpu68020;
        case 68030:
            return kCpu68030;
        case 68040:
            return kCpu68040;
        case 68060:
            return kCpu68060;
        default:
            return 0;
    }
}

std::span<const CpuRegDef> CpuRegisterCatalog() { return kCatalog; }

const CpuRegDef& GetCpuRegister(CpuRegId id) { return kCatalog[static_cast<size_t>(id)]; }

const CpuRegDef* FindCpuRegister(std::string_view name)
{
    if (EqualsIgnoreCase(name, "SSP"))
        return &GetCpuRegister(CpuRegId::Isp);
    if (EqualsIgnoreCase(name, "BUSC"))
        return &GetCpuRegister(CpuRegId::Buscr);
    if (EqualsIgnoreCase(name, "MMUS"))
        return &GetCpuRegister(CpuRegId::Mmusr);
    for (const auto& def : kCatalog)
        if (EqualsIgnoreCase(name, def.name))
            return &def;
    return nullptr;
}

bool CpuHasRegister(const CpuRegDef& def, int cpuModel, int fpuModel)
{
    if ((def.cpuMask & CpuModelBit(cpuModel)) == 0)
        return false;
    if (def.group == CpuRegGroup::Fpu)
        return fpuModel != 0;
    return true;
}

std::string SupportedOnText(const CpuRegDef& def)
{
    static constexpr std::array<std::pair<uint8_t, const char*>, 6> kNames{{
        {kCpu68000, "68000"},
        {kCpu68010, "68010"},
        {kCpu68020, "68020"},
        {kCpu68030, "68030"},
        {kCpu68040, "68040"},
        {kCpu68060, "68060"},
    }};
    std::string text;
    if (def.cpuMask == kCpuAll) {
        text = "every 680x0";
    } else {
        // A contiguous run up to the 68060 reads as "68010+".
        size_t first = 0;
        while (first < kNames.size() && (def.cpuMask & kNames[first].first) == 0)
            ++first;
        uint8_t tail = 0;
        for (size_t i = first; i < kNames.size(); ++i)
            tail |= kNames[i].first;
        if (first < kNames.size() - 1 && def.cpuMask == tail) {
            text = fmt::format("{}+", kNames[first].second);
        } else {
            for (const auto& [bit, name] : kNames) {
                if ((def.cpuMask & bit) == 0)
                    continue;
                if (!text.empty())
                    text += " / ";
                text += name;
            }
        }
    }
    if (def.group == CpuRegGroup::Fpu)
        text += " with an FPU";
    return text;
}

std::string NotOnCpuText(const CpuRegDef& def, int cpuModel)
{
    if ((def.cpuMask & CpuModelBit(cpuModel)) != 0 && def.group == CpuRegGroup::Fpu)
        return fmt::format("{}: no FPU fitted ({})", def.name, SupportedOnText(def));
    return fmt::format("{}: not on a {} ({})", def.name, cpuModel, SupportedOnText(def));
}

const char* IspName(int cpuModel) { return cpuModel < 68020 ? "SSP" : "ISP"; }

std::optional<CpuRegReading> ReadCpuRegister(const CpuExtendedState& st, const CpuRegDef& def)
{
    if (!CpuHasRegister(def, st.cpuModel, st.fpuModel))
        return std::nullopt;

    CpuRegReading r;
    if (IsFpRegister(def.id)) {
        if (!st.fpuPresent)
            return std::nullopt;
        r.fp = &st.fp[static_cast<int>(def.id) - static_cast<int>(CpuRegId::Fp0)];
        r.hexDigits = 24;
        return r;
    }
    switch (def.id) {
        case CpuRegId::Usp:
            r.value = st.usp;
            return r;
        case CpuRegId::Isp:
            r.value = st.isp;
            return r;
        case CpuRegId::Fpcr:
        case CpuRegId::Fpsr:
        case CpuRegId::Fpiar:
            if (!st.fpuPresent)
                return std::nullopt;
            r.value = def.id == CpuRegId::Fpcr   ? st.fpcr
                      : def.id == CpuRegId::Fpsr ? st.fpsr
                                                 : st.fpiar;
            return r;
        default:
            break;
    }

    if (Is030Pmove(def.id, st.cpuModel)) {
        if (!st.mmu030Present)
            return std::nullopt;
        switch (def.id) {
            case CpuRegId::Tc:
                r.value = st.tc030;
                break;
            case CpuRegId::Crp:
                r.value = st.crp030;
                r.hexDigits = 16;
                break;
            case CpuRegId::Srp:
                r.value = st.srp030;
                r.hexDigits = 16;
                break;
            case CpuRegId::Tt0:
                r.value = st.tt0030;
                break;
            case CpuRegId::Tt1:
                r.value = st.tt1030;
                break;
            default:
                r.value = st.mmusr030;
                r.hexDigits = 4;
                break;
        }
        return r;
    }

    const int movec = MovecIndex(def.id);
    if (movec < 0 || (st.movecValid & (1u << movec)) == 0)
        return std::nullopt;
    r.value = def.id == CpuRegId::Msp ? st.msp : st.movec[movec];
    return r;
}

std::string FormatCpuRegister(const CpuRegReading& reading)
{
    if (reading.fp)
        return FormatFpRaw(*reading.fp);
    if (reading.hexDigits == 16)
        return fmt::format("{:08X}:{:08X}", uint32_t(reading.value >> 32), uint32_t(reading.value));
    return fmt::format("{:0{}X}", reading.value, reading.hexDigits);
}

std::string FormatFpRaw(const CpuExtendedState::FpReg& fp)
{
    return fmt::format("{:04X}-{:08X}-{:08X}", fp.ext[0] >> 16, fp.ext[1], fp.ext[2]);
}

std::string FormatFpValue(const CpuExtendedState::FpReg& fp)
{
    std::string_view text = fp.text;
    if (!text.empty() && text.front() == '#')
        text.remove_prefix(1);
    return std::string(text);
}

const char* ActiveStackName(const CpuExtendedState& st)
{
    if (!st.supervisor)
        return "USP";
    if (st.master)
        return "MSP";
    return IspName(st.cpuModel);
}

const char* UaeSettableName(const CpuExtendedState& st, const CpuRegDef& def)
{
    if (!ReadCpuRegister(st, def))
        return nullptr;
    if (def.group == CpuRegGroup::Fpu || Is030Pmove(def.id, st.cpuModel))
        return nullptr;
    // UAE's `r USP` writes the shadow; while that stack is live A7 is the register.
    const bool active = (def.id == CpuRegId::Usp && !st.supervisor) ||
                        (def.id == CpuRegId::Isp && st.supervisor && !st.master) ||
                        (def.id == CpuRegId::Msp && st.supervisor && st.master);
    if (active)
        return "A7";
    switch (def.id) {
        case CpuRegId::Buscr:
            return "BUSC";
        case CpuRegId::Mmusr:
            return "MMUS";
        default:
            return def.name;
    }
}

std::string DecodeFpcr(uint32_t fpcr)
{
    static constexpr const char* kPrec[] = {"X", "S", "D", "?"};
    static constexpr const char* kRound[] = {"RN", "RZ", "RM", "RP"};
    static constexpr const char* kExc[] = {"BSUN", "SNAN", "OPERR", "OVFL",
                                           "UNFL", "DZ",   "INEX2", "INEX1"};
    std::string enabled;
    for (int bit = 15; bit >= 8; --bit) {
        if (fpcr & (1u << bit)) {
            if (!enabled.empty())
                enabled += ' ';
            enabled += kExc[15 - bit];
        }
    }
    return fmt::format("prec {} round {} enable {}", kPrec[(fpcr >> 6) & 3],
                       kRound[(fpcr >> 4) & 3], enabled.empty() ? "-" : enabled);
}

std::string DecodeFpsr(uint32_t fpsr)
{
    static constexpr const char* kExc[] = {"BSUN", "SNAN", "OPERR", "OVFL",
                                           "UNFL", "DZ",   "INEX2", "INEX1"};
    static constexpr const char* kAcc[] = {"IOP", "OVFL", "UNFL", "DZ", "INEX"};
    auto list = [](uint32_t bits, int hi, int lo, const char* const* names) {
        std::string out;
        for (int bit = hi; bit >= lo; --bit) {
            if (bits & (1u << bit)) {
                if (!out.empty())
                    out += ' ';
                out += names[hi - bit];
            }
        }
        return out.empty() ? std::string("-") : out;
    };
    const int quotient = (fpsr >> 16) & 0x7F;
    return fmt::format("N={} Z={} I={} NAN={} Q={}{} exc {} acc {}", (fpsr >> 27) & 1,
                       (fpsr >> 26) & 1, (fpsr >> 25) & 1, (fpsr >> 24) & 1,
                       (fpsr & 0x800000) ? "-" : "", quotient, list(fpsr, 15, 8, kExc),
                       list(fpsr, 7, 3, kAcc));
}

std::string FpuName(int fpuModel, int cpuModel)
{
    if (fpuModel == 0)
        return "None";
    if (fpuModel == 68040 || fpuModel == 68060 || fpuModel == cpuModel)
        return fmt::format("{} internal", fpuModel);
    return std::to_string(fpuModel);
}

std::string CpuMmuName(const CpuExtendedState& st)
{
    if (st.mmuModel == 0)
        return "None";
    if (st.mmuEc)
        return fmt::format("68EC0{}", st.mmuModel % 100);
    return std::to_string(st.mmuModel);
}

} // namespace adm::core
