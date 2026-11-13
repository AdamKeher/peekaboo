#pragma once

#include <cstdint>

namespace adm::core {

#pragma pack(push, 2)

// Based on standard Amiga NDK custom.h
struct Custom
{
    volatile uint16_t bltddat;
    volatile uint16_t dmaconr;
    union {
        volatile uint32_t vpos32;
        struct
        {
            union {
                volatile uint16_t vposr;
                struct
                {
                    volatile uint8_t vposr_h;
                    volatile uint8_t vposr_l;
                };
            };
            union {
                volatile uint16_t vhposr;
                struct
                {
                    volatile uint8_t vhposr_h;
                    volatile uint8_t vhposr_l;
                };
            };
        };
    };
    volatile uint16_t dskdatr;
    volatile uint16_t joy0dat;
    volatile uint16_t joy1dat;
    volatile uint16_t clxdat;
    volatile uint16_t adkconr;
    volatile uint16_t pot0dat;
    volatile uint16_t pot1dat;
    volatile uint16_t potinp;
    volatile uint16_t serdatr;
    volatile uint16_t dskbytr;
    volatile uint16_t intenar;
    volatile uint16_t intreqr;
    volatile uint32_t dskpt;
    volatile uint16_t dsklen;
    volatile uint16_t dskdat;
    volatile uint16_t refptr;
    volatile uint16_t vposw;
    volatile uint16_t vhposw;
    volatile uint16_t copcon;
    volatile uint16_t serdat;
    volatile uint16_t serper;
    volatile uint16_t potgo;
    volatile uint16_t joytest;
    volatile uint16_t strequ;
    volatile uint16_t strvbl;
    volatile uint16_t strhor;
    volatile uint16_t strlong;
    volatile uint16_t bltcon0;
    volatile uint16_t bltcon1;
    volatile uint16_t bltafwm;
    volatile uint16_t bltalwm;
    volatile uint32_t bltcpt;
    volatile uint32_t bltbpt;
    volatile uint32_t bltapt;
    volatile uint32_t bltdpt;
    volatile uint16_t bltsize;
    volatile uint8_t pad2d;
    volatile uint8_t bltcon0l; /* low 8 bits of bltcon0, write only */
    volatile uint16_t bltsizv;
    volatile uint16_t bltsizh; /* 5e */
    volatile uint16_t bltcmod;
    volatile uint16_t bltbmod;
    volatile uint16_t bltamod;
    volatile uint16_t bltdmod;
    volatile uint16_t pad34[4];
    volatile uint16_t bltcdat;
    volatile uint16_t bltbdat;
    volatile uint16_t bltadat;
    volatile uint16_t pad3b[3];
    volatile uint16_t deniseid; /* 7c */
    volatile uint16_t dsksync;
    volatile uint32_t cop1lc;
    volatile uint32_t cop2lc;
    volatile uint16_t copjmp1;
    volatile uint16_t copjmp2;
    volatile uint16_t copins;
    volatile uint16_t diwstrt;
    volatile uint16_t diwstop;
    volatile uint16_t ddfstrt;
    volatile uint16_t ddfstop;
    volatile uint16_t dmacon;
    volatile uint16_t clxcon;
    volatile uint16_t intena;
    volatile uint16_t intreq;
    volatile uint16_t adkcon;
    struct AudChannel
    {
        volatile uint32_t ac_ptr;    /* ptr to start of waveform data */
        volatile uint16_t ac_len;    /* length of waveform in words */
        volatile uint16_t ac_per;    /* sample period */
        volatile uint16_t ac_vol;    /* volume */
        volatile uint16_t ac_dat;    /* sample pair */
        volatile uint16_t ac_pad[2]; /* unused */
    } aud[4];
    volatile uint32_t bplpt[8];
    volatile uint16_t bplcon0;
    volatile uint16_t bplcon1;
    volatile uint16_t bplcon2;
    volatile uint16_t bplcon3;
    volatile uint16_t bpl1mod;
    volatile uint16_t bpl2mod;
    volatile uint16_t bplcon4;
    volatile uint16_t clxcon2;
    volatile uint16_t bpldat[8];
    volatile uint32_t sprpt[8];
    struct SpriteDef
    {
        volatile uint16_t pos;
        volatile uint16_t ctl;
        volatile uint16_t dataa;
        volatile uint16_t datab;
    } spr[8];
    volatile uint16_t color[32];
    volatile uint16_t htotal;
    volatile uint16_t hsstop;
    volatile uint16_t hbstrt;
    volatile uint16_t hbstop;
    volatile uint16_t vtotal;
    volatile uint16_t vsstop;
    volatile uint16_t vbstrt;
    volatile uint16_t vbstop;
    volatile uint16_t sprhstrt;
    volatile uint16_t sprhstop;
    volatile uint16_t bplhstrt;
    volatile uint16_t bplhstop;
    volatile uint16_t hhposw;
    volatile uint16_t hhposr;
    volatile uint16_t beamcon0;
    volatile uint16_t hsstrt;
    volatile uint16_t vsstrt;
    volatile uint16_t hcenter;
    volatile uint16_t diwhigh; /* 1e4 */
    volatile uint16_t padf3[11];
    volatile uint16_t fmode;
    volatile uint16_t pad1fe;
};

#pragma pack(pop)

static_assert(sizeof(Custom) == 512, "Custom size must be 512 bytes");

// Custom-chip 32-bit pointer registers (cop1lc/cop2lc/bplpt[]/sprpt[]) are stored word-swapped in
// the Custom snapshot (the emulator memcpy lands the low word in the high half). This swaps the two
// 16-bit halves back without masking; use DecodeAmigaPtr() when you want the 24-bit Amiga address.
inline uint32_t SwapPtrWords(uint32_t v) { return (v << 16) | (v >> 16); }

// Normalize a word-swapped custom-chip pointer register to a 24-bit Amiga address.
inline uint32_t DecodeAmigaPtr(uint32_t v) { return SwapPtrWords(v) & 0x00FFFFFFu; }

// CIA chip register snapshot (one per chip: CIAA and CIAB)
struct CIAChip
{
    uint8_t pra;    // Peripheral Data Register A
    uint8_t prb;    // Peripheral Data Register B
    uint8_t dra;    // Data Direction Register A
    uint8_t drb;    // Data Direction Register B
    uint8_t talo;   // Timer A counter low byte
    uint8_t tahi;   // Timer A counter high byte
    uint8_t tblo;   // Timer B counter low byte
    uint8_t tbhi;   // Timer B counter high byte
    uint8_t todlo;  // Time of Day counter bits 7-0
    uint8_t todmid; // Time of Day counter bits 15-8
    uint8_t todhi;  // Time of Day counter bits 23-16
    uint8_t sdr;    // Serial Data Register
    uint8_t icr;    // Interrupt Control Register (read = IR flags)
    uint8_t imask;  // Interrupt mask (ICR write mask)
    uint8_t cra;    // Control Register A
    uint8_t crb;    // Control Register B
};

struct CIAState
{
    CIAChip a; // CIAA (base 0xBFE001, odd byte)
    CIAChip b; // CIAB (base 0xBFD000, even byte)
};

// Where the display window opens, from DIWSTRT's vstart, with PAL's usual $2C as the fallback.
// `diwstrt` is a write shadow, so it reads 0 on a machine that has not written the register since
// the snapshot was built; a vstart no display could follow is treated the same way.
constexpr int kDefaultDiwVStart = 0x2C;
inline int DisplayWindowFirstLine(const Custom& custom)
{
    const int vstart = (custom.diwstrt >> 8) & 0xFF;
    return (vstart > 0 && vstart < 240) ? vstart : kDefaultDiwVStart;
}

// Has the beam already drawn over the visible part of the field sitting in the framebuffer?
//
// The buffer holds the previous field below wherever the beam stands and the new one above it, so
// a capture is only really partial once the beam has descended into the display window: above
// that line nothing visible has been overwritten and the buffer holds a whole field.
//
// "vpos != 0" is not that test, which is what the screenshot and get_status tools used to ask. A
// completed frame leaves the beam a few lines into the next field's vertical blank - measured at
// vpos 3 on a PAL A500, never 0 - so every paused capture came back partial, and the advice to
// call end_frame could never clear it because end_frame lands on vpos 3 too. See EndFrameTest.
inline bool BeamHasOverwrittenVisibleField(const Custom& custom, int vpos)
{
    return vpos > DisplayWindowFirstLine(custom);
}

} // namespace adm::core
