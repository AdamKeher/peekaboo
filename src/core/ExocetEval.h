#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace adm::core {

// Peekaboo's port of the Exocet evaluator: the crass repo's tools/exocet.py `Runtime`, which
// engine/exocet.c also follows, step for step. The rules are that file's; this is them in C++,
// held to the vectors exocet.py writes (tests/data/exocet/), bit for bit, the way the engine is.
//
// Values are integers throughout: a fix track's are 16.16. An interpolation's shape works on the
// top byte of the fraction, and a value is v0 + (((v1 - v0) * s + 0x8000) >> 16) with an
// arithmetic shift - Python's, which C++20 also guarantees. A colour track's value is 0xRRGGBBAA
// (held in the int32 with those bits) and each byte is interpolated that way on its own.

enum class ExocetType : uint8_t { Fix = 1, Int = 2, Index = 3, Ptr = 4, Event = 5, Color = 6 };

enum class ExocetInterp : uint8_t { Step = 0, Linear = 1, Smooth = 2, Ramp = 3 };

struct ExocetKey
{
    uint16_t row = 0; // played row
    uint8_t interp = 0;
    int32_t value = 0; // as exocet.bin holds it: 16.16 for a fix track
    uint8_t sub = 0;   // 256ths into the row: tick t of a row of n ticks is t * 256 / n
    // The key's time in 65536ths of a row: its row, and its sub in 256ths (engine/exocet.c exo_at)
    uint32_t At() const
    {
        return (static_cast<uint32_t>(row) << 16) | (static_cast<uint32_t>(sub) << 8);
    }
};

// A key's sub for tick `tick` of a row `frames` ticks long (exocet.py key_sub).
inline uint8_t ExocetSub(uint32_t tick, uint32_t frames)
{
    return frames ? static_cast<uint8_t>((tick << 8) / frames) : 0;
}

// The fraction the runtime works out on tick `tick` of a row `frames` ticks long.
inline uint16_t ExocetTickFrac(uint32_t tick, uint32_t frames)
{
    return frames ? static_cast<uint16_t>((tick << 16) / frames) : 0;
}

// How far `t` is into a span, both in 65536ths of a row, in 65536ths (exocet.py span_fraction).
uint32_t ExocetSpanFraction(uint32_t t, uint32_t span);

struct ExocetBinTrack
{
    uint8_t type = 0;
    uint8_t group = 0;
    int32_t def = 0;
    std::vector<ExocetKey> keys;
};

struct ExocetSpan
{
    uint16_t row = 0;    // first played row of the run
    uint16_t frame = 0;  // the frame it starts on
    uint16_t length = 0; // frames a row
};

// exocet.bin, parsed (the crass doc, "exocet.bin").
struct ExocetBin
{
    uint32_t manifest = 0;
    uint32_t song = 0; // the first song's trigger digest
    uint32_t main = 0; // the main song's, 0 with one song
    uint16_t rows = 0;
    uint16_t frames = 0; // the frame after the song's last row
    std::vector<ExocetBinTrack> tracks;
    std::vector<ExocetSpan> spans;

    bool Parse(const uint8_t* data, size_t size, std::string* error = nullptr);
};

// How far along a span `u` (0..65535 of it) is, in 65536ths, for an interpolation.
uint32_t ExocetShape(uint8_t interp, uint32_t u);

// A value track's value at (row, frac) from its keys (sorted by row) alone - what the runtime
// reads there after any update that reached it. For the grid and the graph.
int32_t ExocetValueAt(const std::vector<ExocetKey>& keys, int32_t def, uint32_t row, uint16_t frac,
                      ExocetType type = ExocetType::Int);

// A colour track's value `s` (65536ths) of the way from one key's to the next, byte by byte.
int32_t ExocetBlend(int32_t v0, int32_t v1, uint32_t s);

// The runtime: exo_init, exo_enable, exo_update and the fallback clock.
class ExocetRuntime
{
public:
    explicit ExocetRuntime(ExocetBin bin);

    void SetDirector(int track);
    void Enable(int group, bool on);
    void Update(uint32_t row, uint16_t frac);
    // The fallback clock: (row, frac) for a song frame, through the runs of rows.
    std::pair<uint32_t, uint16_t> Clock(uint32_t frame);
    void UpdateFrame(uint32_t frame);
    // The frame a played row starts on (the song's end past its last row).
    uint32_t RowFrame(uint32_t row) const;
    bool CutReached(int cut) const;

    const std::vector<int32_t>& Values() const { return m_values; }
    uint32_t Reached() const { return m_reached; }
    uint32_t Enabled() const { return m_enabled; }
    uint32_t Row() const { return m_row; }
    uint16_t Frac() const { return m_frac; }
    const ExocetBin& Bin() const { return m_bin; }

private:
    ExocetBin m_bin;
    std::vector<int32_t> m_values;
    std::vector<int> m_cur;
    std::vector<bool> m_seek;
    uint32_t m_enabled = 0;
    uint32_t m_row = 0;
    uint16_t m_frac = 0;
    size_t m_frameSpan = 0;
    uint32_t m_reached = 0;
    int m_director = -1;
    uint32_t m_keyed = 0;
};

// A key's value as the file writes it and as exocet.bin holds it: 16.16 for a fix track,
// rounded half to even as Python's round() does; a colour's 0xRRGGBBAA (0..4294967295) as the
// int32 with those bits; the number itself for the rest.
int32_t ExocetToBin(ExocetType type, double value);
double ExocetFromBin(ExocetType type, int32_t value);

// "step", "linear", "smooth", "ramp" -> 0..3; -1 for anything else.
int ExocetInterpFromName(const std::string& name);
const char* ExocetInterpName(uint8_t interp);
// "fix", "int", "index", "ptr", "event", "color" -> ExocetType; 0 for anything else.
uint8_t ExocetTypeFromName(const std::string& name);

} // namespace adm::core
