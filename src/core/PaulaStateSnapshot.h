#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace adm::core {

// Chipset-side view of Paula's four audio voices, produced on demand by Application and consumed
// by the AR console PAULA command and the MCP get_paula_state tool.
//
// AudioLogSnapshot is the sibling of this type and answers a different question: it measures the
// HOST path (ring buffer, resampler, sound card). When something sounds wrong, this snapshot says
// what the Amiga asked for and that one says what the machine did with it.

// Paula's sample clock. A channel emits one sample every `period` colour clocks.
inline constexpr double kPaulaClockPal = 3546895.0;
inline constexpr double kPaulaClockNtsc = 3579545.0;

// Playback rate for an AUDxPER value. Period 0 and 1 are illegal on real hardware (Paula clamps
// below ~124), so anything under 2 is reported as silence rather than a meaningless megahertz.
inline double PaulaPeriodToHz(uint16_t period, bool pal = true)
{
    if (period < 2)
        return 0.0;
    return (pal ? kPaulaClockPal : kPaulaClockNtsc) / static_cast<double>(period);
}

// ProTracker-style note name for a period, e.g. 428 -> "C-2", 254 -> "A-2".
//
// This names the PERIOD, not the pitch you hear: what a voice actually sounds like also depends on
// the sample loaded into it. It is the same mapping every Amiga tracker shows, which is what makes
// it useful for matching a channel against a pattern row.
//
// Derived rather than table-driven so periods outside ProTracker's three octaves still resolve.
// C-1 = 856 is the anchor; cents is the offset from the tempered note (-50..+50).
inline std::string PaulaPeriodToNote(uint16_t period, int* centsOut = nullptr)
{
    if (centsOut)
        *centsOut = 0;
    if (period < 2)
        return "--";
    static const char* kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};
    const double semis = 12.0 * std::log2(856.0 / static_cast<double>(period));
    const int idx = static_cast<int>(std::lround(semis));
    if (centsOut)
        *centsOut = static_cast<int>(std::lround((semis - idx) * 100.0));

    // Floor-divide so periods above C-1 (negative semitone offsets) still land in the right octave.
    int noteIdx = idx % 12;
    int octave = idx / 12;
    if (noteIdx < 0) {
        noteIdx += 12;
        octave -= 1;
    }
    // Trackers pad every note to three columns: naturals borrow a '-' ("C-2"), sharps use the '#'
    // as their own padding ("C#2"). Always inserting the '-' would render "C#-2".
    const std::string name = kNames[noteIdx];
    return name + (name.size() == 1 ? "-" : "") + std::to_string(octave + 1);
}

struct PaulaChannel
{
    int index = 0;
    uint32_t lc = 0;  // AUDxLC, normalised to a 24-bit Amiga address
    uint16_t len = 0; // AUDxLEN, in words
    uint16_t per = 0; // AUDxPER
    uint16_t vol = 0; // AUDxVOL (0-64)
    double hz = 0.0;
    std::string note;

    bool dmaOn = false;        // DMACON channel bit AND the DMAEN master bit
    bool audible = false;      // not muted by the Audio window's channel mask
    bool intPending = false;   // INTREQ bit 0x80 << n
    bool intEnabled = false;   // INTENA bit 0x80 << n
    bool attachVolume = false; // ADKCON: this channel modulates channel n+1's volume
    bool attachPeriod = false; // ADKCON: ... its period

    // Measured from the Paula tap over the most recent frame. Zero when the core lacks the tap.
    int sampleCount = 0;
    float peak = 0.0f; // 0..1, post-volume
    float rms = 0.0f;  // 0..1, post-volume
};

struct PaulaStateSnapshot
{
    bool available = false;      // emulator present and running a disk
    bool traceAvailable = false; // core exports the audio tap (peak/rms/sampleCount are real)
    bool masterDmaOn = false;    // DMACON DMAEN
    bool hostMuted = false;      // the app's own master mute, not a chipset state

    uint8_t channelMask = 0x0F; // bit n = channel n audible
    bool ledFilterOn = false;   // the one-pole LED filter is in circuit right now
    int filterModel = 0;        // 0 none, 1 A500, 2 A1200, 3 A500 fixed-only

    std::array<PaulaChannel, 4> channels{};
};

} // namespace adm::core
