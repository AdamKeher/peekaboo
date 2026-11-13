#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace adm::core {

// Byte-for-byte mirror of the core's struct adm_cpu_ext_state (patch 1380, core 1.102.0).
// movec[] follows UAE's m2cregs[] order: SFC DFC CACR TC ITT0 ITT1 DTT0 DTT1 BUSC USP VBR CAAR
// MSP ISP MMUS URP SRP PCR.
struct CpuExtRaw
{
    uint32_t structSize;
    uint32_t cpuModel, fpuModel, mmuModel, mmuEc;
    uint32_t s, m, intmask, stopped;
    uint32_t usp, isp, msp;
    uint32_t movecValid;
    uint32_t movec[18];
    uint32_t fpuPresent;
    uint32_t fpExt[8][3];
    uint32_t fpcr, fpsr, fpiar;
    uint32_t mmu030Present;
    uint32_t crpHi, crpLo, srpHi, srpLo;
    uint32_t tc030, tt0030, tt1030, mmusr030;
    char fpText[8][40];
};
static_assert(sizeof(CpuExtRaw) == 68 * 4 + 8 * 40);

// Every CPU register beyond D0-D7/A0-A7/PC/SR, as the core reports it for the configured CPU.
struct CpuExtendedState
{
    int cpuModel = 68000; // 68000 ... 68060
    int fpuModel = 0;     // 0, 68881, 68882, 68040, 68060
    int mmuModel = 0;     // 0, 68030, 68040, 68060
    bool mmuEc = false;

    bool supervisor = false;
    bool master = false; // SR M bit (68020/030)
    int intmask = 0;
    bool stopped = false;

    uint32_t usp = 0, isp = 0, msp = 0; // A7 already folded into the active one
    uint32_t movecValid = 0;
    std::array<uint32_t, 18> movec{};

    bool fpuPresent = false;
    struct FpReg
    {
        uint32_t ext[3]{}; // 96-bit extended, FMOVEM layout (sign/exponent in ext[0] >> 16)
        std::string text;  // decimal, as UAE prints it
    };
    std::array<FpReg, 8> fp{};
    uint32_t fpcr = 0, fpsr = 0, fpiar = 0;

    bool mmu030Present = false;
    uint64_t crp030 = 0, srp030 = 0;
    uint32_t tc030 = 0, tt0030 = 0, tt1030 = 0;
    uint16_t mmusr030 = 0;
};

std::optional<CpuExtendedState> ParseCpuExtState(const CpuExtRaw& raw, int returned);

// CPU model bits for the register catalogue.
enum CpuBit : uint8_t {
    kCpu68000 = 1 << 0,
    kCpu68010 = 1 << 1,
    kCpu68020 = 1 << 2,
    kCpu68030 = 1 << 3,
    kCpu68040 = 1 << 4,
    kCpu68060 = 1 << 5,
    kCpuAll = 0x3F,
};
uint8_t CpuModelBit(int cpuModel); // 68030 -> kCpu68030; 0 for anything else

enum class CpuRegGroup {
    Stack,
    System,
    Mmu,
    Fpu,
};

enum class CpuRegId {
    Usp,
    Isp,
    Msp,
    Vbr,
    Sfc,
    Dfc,
    Cacr,
    Caar,
    Buscr,
    Pcr,
    Tc,
    Crp,
    Srp,
    Urp,
    Tt0,
    Tt1,
    Itt0,
    Itt1,
    Dtt0,
    Dtt1,
    Mmusr,
    Fp0,
    Fp1,
    Fp2,
    Fp3,
    Fp4,
    Fp5,
    Fp6,
    Fp7,
    Fpcr,
    Fpsr,
    Fpiar,
};

struct CpuRegDef
{
    CpuRegId id;
    const char* name;
    CpuRegGroup group;
    uint8_t cpuMask;
    bool isAddress; // a pointer worth a memory link
    const char* description;
};

// Every register, in display order within its group.
std::span<const CpuRegDef> CpuRegisterCatalog();
const CpuRegDef& GetCpuRegister(CpuRegId id);
// Case-insensitive; also accepts SSP (ISP), BUSC (BUSCR) and MMUS (MMUSR).
const CpuRegDef* FindCpuRegister(std::string_view name);

// Whether the configured machine has this register at all. FPU registers need an FPU as well as
// a CPU that takes one; nothing else depends on more than the CPU.
bool CpuHasRegister(const CpuRegDef& def, int cpuModel, int fpuModel);
// "68010+", "68020 / 68030", "68060", "68020+ with an FPU".
std::string SupportedOnText(const CpuRegDef& def);
// "VBR: not on a 68000 (68010+)".
std::string NotOnCpuText(const CpuRegDef& def, int cpuModel);
// Label for the ISP on this CPU: "SSP" on a 68000/010 (one supervisor stack), "ISP" otherwise.
const char* IspName(int cpuModel);

// One register's value read out of the state. Absent when the machine lacks the register.
struct CpuRegReading
{
    uint64_t value = 0;
    int hexDigits = 8; // 4, 8 or 16; FP registers use 24 and value holds nothing
    const CpuExtendedState::FpReg* fp = nullptr;
};
std::optional<CpuRegReading> ReadCpuRegister(const CpuExtendedState& state, const CpuRegDef& def);
std::string FormatCpuRegister(const CpuRegReading& reading);  // hex, hi:lo for 64-bit
std::string FormatFpRaw(const CpuExtendedState::FpReg& fp);   // "4000-C90FDAA2-2168C235"
std::string FormatFpValue(const CpuExtendedState::FpReg& fp); // decimal, UAE's leading '#' removed

// Which stack A7 is: "USP", "ISP"/"SSP" or "MSP".
const char* ActiveStackName(const CpuExtendedState& state);

// The UAE debugger name for `r <name> <value>`, or nullptr when UAE cannot set this register
// on this CPU (FPU registers, the 68030's PMOVE registers). Set on the active stack means A7.
const char* UaeSettableName(const CpuExtendedState& state, const CpuRegDef& def);

// FPCR/FPSR decoded as words: "RN X" ; "CC=N- Z- I- NAN- Q=00 EXC=.. AEXC=..".
std::string DecodeFpcr(uint32_t fpcr);
std::string DecodeFpsr(uint32_t fpsr);

// "68030", "68882", "CPU internal" / "None".
std::string FpuName(int fpuModel, int cpuModel);
std::string CpuMmuName(const CpuExtendedState& state); // "None", "68030", "68EC030", "68040"

} // namespace adm::core
