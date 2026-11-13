#include "ExocetEval.h"

#include <algorithm>
#include <cmath>

namespace adm::core {
namespace {

constexpr uint32_t kMagic = 0x45584F31u; // "EXO1"
constexpr uint16_t kVersion = 3;         // 3: the main song's digest (2: a key's sub)
constexpr size_t kHead = 40, kTrack = 12, kKey = 8, kSpan = 8;

uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint32_t Be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool Fail(std::string* error, const char* what)
{
    if (error)
        *error = what;
    return false;
}

int32_t Lerp(int32_t v0, int32_t v1, uint32_t s)
{
    const int64_t d = static_cast<int64_t>(v1) - v0;
    return static_cast<int32_t>(v0 + ((d * s + 0x8000) >> 16));
}

// A value track at its cursor `c` (the last key at or before the row; -1 before the first).
int32_t ValueFrom(const std::vector<ExocetKey>& keys, int32_t def, int c, uint32_t row,
                  uint16_t frac, bool color)
{
    if (c < 0)
        return def;
    const ExocetKey& k0 = keys[c];
    if (c + 1 == static_cast<int>(keys.size()) || k0.interp == 0)
        return k0.value;
    const ExocetKey& k1 = keys[c + 1];
    const uint32_t now = (row << 16) | frac;
    const uint32_t u = ExocetSpanFraction(now - k0.At(), k1.At() - k0.At());
    const uint32_t s = ExocetShape(k0.interp, u);
    return color ? ExocetBlend(k0.value, k1.value, s) : Lerp(k0.value, k1.value, s);
}

} // namespace

bool ExocetBin::Parse(const uint8_t* d, size_t size, std::string* error)
{
    *this = ExocetBin{};
    if (size < kHead || Be32(d) != kMagic || Be16(d + 4) != kVersion)
        return Fail(error, "not an exocet.bin (EXO1, version 3)");
    const uint16_t n = Be16(d + 6);
    manifest = Be32(d + 8);
    song = Be32(d + 12);
    rows = Be16(d + 16);
    const uint16_t nkeys = Be16(d + 18);
    const uint16_t nspans = Be16(d + 20);
    frames = Be16(d + 22);
    const uint32_t trackOff = Be32(d + 24), keyOff = Be32(d + 28), spanOff = Be32(d + 32);
    main = Be32(d + 36);
    if (trackOff + uint64_t(n) * kTrack > size || keyOff + uint64_t(nkeys) * kKey > size ||
        spanOff + uint64_t(nspans) * kSpan > size)
        return Fail(error, "exocet.bin: a table runs past the end of the file");
    for (uint16_t i = 0; i < n; ++i) {
        const uint8_t* t = d + trackOff + kTrack * i;
        const uint16_t first = Be16(t), count = Be16(t + 2);
        if (first + count > nkeys)
            return Fail(error, "exocet.bin: a track's keys run past the key table");
        ExocetBinTrack tr;
        tr.type = t[4];
        tr.group = t[5];
        tr.def = static_cast<int32_t>(Be32(t + 8));
        for (uint16_t k = first; k < first + count; ++k) {
            const uint8_t* p = d + keyOff + kKey * k;
            tr.keys.push_back({Be16(p), p[2], static_cast<int32_t>(Be32(p + 4)), p[3]});
        }
        tracks.push_back(std::move(tr));
    }
    for (uint16_t s = 0; s < nspans; ++s) {
        const uint8_t* p = d + spanOff + kSpan * s;
        spans.push_back({Be16(p), Be16(p + 2), Be16(p + 4)});
    }
    return true;
}

uint32_t ExocetSpanFraction(uint32_t t, uint32_t span)
{
    if (!(span & 0xFFFF))
        return t / (span >> 16);
    span >>= 8;
    while (span >> 16) {
        span >>= 1;
        t >>= 1;
    }
    return (t << 8) / span;
}

uint32_t ExocetShape(uint8_t interp, uint32_t u)
{
    if (interp == 1)
        return u;
    const uint32_t i = u >> 8;
    if (interp == 2)
        return (i * i * (768 - 2 * i)) >> 8;
    return i * i;
}

int32_t ExocetValueAt(const std::vector<ExocetKey>& keys, int32_t def, uint32_t row, uint16_t frac,
                      ExocetType type)
{
    const uint32_t now = (row << 16) | frac;
    // the last key at or before now (keys are in At() order): a binary search, as the proof
    // and the graphs evaluate every recorded frame
    const auto after = std::upper_bound(keys.begin(), keys.end(), now,
                                        [](uint32_t t, const ExocetKey& k) { return t < k.At(); });
    const int c = static_cast<int>(after - keys.begin()) - 1;
    return ValueFrom(keys, def, c, row, frac, type == ExocetType::Color);
}

int32_t ExocetBlend(int32_t v0, int32_t v1, uint32_t s)
{
    const uint32_t a = static_cast<uint32_t>(v0), b = static_cast<uint32_t>(v1);
    uint32_t out = 0;
    for (int sh = 24; sh >= 0; sh -= 8) {
        const int32_t c0 = (a >> sh) & 255, c1 = (b >> sh) & 255;
        out |= static_cast<uint32_t>(Lerp(c0, c1, s)) << sh;
    }
    return static_cast<int32_t>(out);
}

ExocetRuntime::ExocetRuntime(ExocetBin bin) : m_bin(std::move(bin))
{
    const size_t n = m_bin.tracks.size();
    for (const auto& t : m_bin.tracks)
        m_values.push_back(t.def);
    m_cur.assign(n, -1);
    m_seek.assign(n, true);
}

void ExocetRuntime::SetDirector(int track)
{
    m_director = track;
    m_keyed = 0;
    if (track >= 0 && track < static_cast<int>(m_bin.tracks.size()))
        for (const auto& k : m_bin.tracks[track].keys)
            m_keyed |= 1u << (k.value & 31);
}

void ExocetRuntime::Enable(int group, bool on)
{
    const uint32_t bit = 1u << group;
    if (on && !(m_enabled & bit))
        for (size_t i = 0; i < m_bin.tracks.size(); ++i)
            if (m_bin.tracks[i].group == group)
                m_seek[i] = true;
    m_enabled = on ? (m_enabled | bit) : (m_enabled & ~bit);
}

bool ExocetRuntime::CutReached(int cut) const
{
    const uint32_t bit = 1u << cut;
    return !(m_keyed & bit) || (m_reached & bit);
}

void ExocetRuntime::Update(uint32_t row, uint16_t frac)
{
    const bool back = row < m_row || (row == m_row && frac < m_frac);
    m_row = row;
    m_frac = frac;
    const uint32_t now = (row << 16) | frac;
    for (size_t i = 0; i < m_bin.tracks.size(); ++i) {
        const ExocetBinTrack& t = m_bin.tracks[i];
        if (!(m_enabled & (1u << t.group)))
            continue;
        const auto& keys = t.keys;
        const int nk = static_cast<int>(keys.size());
        const bool director = static_cast<int>(i) == m_director;
        if (t.type == static_cast<uint8_t>(ExocetType::Event)) {
            if (m_seek[i] || back) {
                int c = 0;
                if (director)
                    m_reached = 0;
                while (c < nk && keys[c].At() < now) {
                    if (director)
                        m_reached |= 1u << (keys[c].value & 31);
                    ++c;
                }
                m_cur[i] = c;
                m_seek[i] = false;
            }
            const int c = m_cur[i];
            if (c < nk && keys[c].At() <= now) {
                m_values[i] = keys[c].value;
                m_cur[i] = c + 1;
                if (director)
                    m_reached |= 1u << (keys[c].value & 31);
            } else {
                m_values[i] = 0;
            }
            continue;
        }
        int c = m_cur[i];
        if (m_seek[i] || (c >= 0 && keys[c].At() > now)) {
            c = -1;
            m_seek[i] = false;
        }
        while (c + 1 < nk && keys[c + 1].At() <= now)
            ++c;
        m_cur[i] = c;
        m_values[i] =
            ValueFrom(keys, t.def, c, row, frac, t.type == static_cast<uint8_t>(ExocetType::Color));
    }
}

std::pair<uint32_t, uint16_t> ExocetRuntime::Clock(uint32_t frame)
{
    if (frame >= m_bin.frames || m_bin.spans.empty())
        return {m_bin.rows, 0};
    size_t s = m_frameSpan;
    if (s >= m_bin.spans.size() || m_bin.spans[s].frame > frame)
        s = 0;
    while (s + 1 < m_bin.spans.size() && m_bin.spans[s + 1].frame <= frame)
        ++s;
    m_frameSpan = s;
    const ExocetSpan& sp = m_bin.spans[s];
    const uint32_t d = frame - sp.frame;
    return {sp.row + d / sp.length, static_cast<uint16_t>(((d % sp.length) << 16) / sp.length)};
}

void ExocetRuntime::UpdateFrame(uint32_t frame)
{
    const auto [row, frac] = Clock(frame);
    Update(row, frac);
}

uint32_t ExocetRuntime::RowFrame(uint32_t row) const
{
    if (row >= m_bin.rows)
        return m_bin.frames;
    const ExocetSpan* best = nullptr;
    for (const auto& sp : m_bin.spans)
        if (sp.row <= row && (!best || sp.row > best->row))
            best = &sp;
    if (!best)
        return 0;
    return best->frame + (row - best->row) * best->length;
}

int32_t ExocetToBin(ExocetType type, double value)
{
    if (type == ExocetType::Fix)
        return static_cast<int32_t>(std::nearbyint(value * 65536.0)); // ties to even, as round()
    if (type == ExocetType::Color)
        return static_cast<int32_t>(static_cast<uint32_t>(std::clamp(value, 0.0, 4294967295.0)));
    return static_cast<int32_t>(value);
}

double ExocetFromBin(ExocetType type, int32_t value)
{
    if (type == ExocetType::Color)
        return static_cast<double>(static_cast<uint32_t>(value));
    return type == ExocetType::Fix ? value / 65536.0 : static_cast<double>(value);
}

int ExocetInterpFromName(const std::string& name)
{
    if (name == "step")
        return 0;
    if (name == "linear")
        return 1;
    if (name == "smooth")
        return 2;
    if (name == "ramp")
        return 3;
    return -1;
}

const char* ExocetInterpName(uint8_t interp)
{
    switch (interp) {
        case 1:
            return "linear";
        case 2:
            return "smooth";
        case 3:
            return "ramp";
        default:
            return "step";
    }
}

uint8_t ExocetTypeFromName(const std::string& name)
{
    if (name == "fix")
        return 1;
    if (name == "int")
        return 2;
    if (name == "index")
        return 3;
    if (name == "ptr")
        return 4;
    if (name == "event")
        return 5;
    if (name == "color")
        return 6;
    return 0;
}

} // namespace adm::core
