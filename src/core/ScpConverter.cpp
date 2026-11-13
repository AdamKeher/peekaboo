#include "ScpConverter.h"
#include "BigEndian.h"

#include "AmigaMfmDecoder.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>

namespace adm::core {

namespace {

constexpr int kSectorsPerTrack = AmigaMfmDecoder::kSectorsPerTrack; // 11 (DD)
constexpr int kSectorSize = AmigaMfmDecoder::kSectorSize;           // 512
constexpr int kAdfCylinders = 80;
constexpr int kAdfTracks = kAdfCylinders * 2; // 160

// SCP file header layout (offsets from the start of the file).
constexpr size_t kHeaderBytes = 0x10;
constexpr size_t kTrackTableEntries = 168; // fixed-size table, always 168 longs
constexpr size_t kTrackTableBytes = kTrackTableEntries * 4;

// The SuperCard Pro samples flux at 25 ns per tick at resolution 0; the
// resolution byte multiplies that base period.
constexpr uint32_t kBaseNsPerTick = 25;

// Data-separator constants, straight from scp.c: a nominal 2 us DD bit cell and
// a PLL allowed to pull +/-10% around it.
constexpr int kClockCentreNs = 2000;
constexpr int kClockMaxAdjPercent = 10;

uint32_t Le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t Be16(const uint8_t* p) { return be::U16(p); }

struct ScpHeader
{
    uint8_t revolutions = 0;
    uint8_t startTrack = 0;
    uint8_t endTrack = 0;
    uint32_t nsPerTick = kBaseNsPerTick;
};

bool ParseHeader(const std::vector<uint8_t>& d, ScpHeader& out)
{
    if (d.size() < kHeaderBytes + kTrackTableBytes || std::memcmp(d.data(), "SCP", 3) != 0)
        return false;

    const uint8_t revolutions = d[5];
    const uint8_t startTrack = d[6];
    const uint8_t endTrack = d[7];
    const uint8_t bitCellWidth = d[9];
    const uint8_t resolution = d[11];

    if (revolutions == 0) {
        Logger::Error("SCP: header declares 0 revolutions");
        return false;
    }
    // 0 means the default 16-bit flux entries; scp.c rejects anything else and
    // so do we, because the entry stride would differ.
    if (bitCellWidth != 0 && bitCellWidth != 16) {
        Logger::Error("SCP: unsupported bit cell width {}", bitCellWidth);
        return false;
    }
    if (endTrack < startTrack || endTrack >= kTrackTableEntries) {
        Logger::Error("SCP: implausible track range {}..{}", startTrack, endTrack);
        return false;
    }

    out.revolutions = revolutions;
    out.startTrack = startTrack;
    out.endTrack = endTrack;
    out.nsPerTick = kBaseNsPerTick * (static_cast<uint32_t>(resolution) + 1);
    return true;
}

// One stored revolution of a track: where its flux entries live in the file.
struct RevSpan
{
    size_t offset = 0; // byte offset of the first flux entry
    size_t count = 0;  // number of 16-bit flux entries
};

// Locate a track's data header and read its per-revolution spans. Returns false
// when the track is absent, the "TRK" magic is wrong, or a span runs past EOF.
bool ReadTrackSpans(const std::vector<uint8_t>& d, const ScpHeader& hdr, int trackIndex,
                    std::vector<RevSpan>& out)
{
    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= kTrackTableEntries)
        return false;

    const uint32_t tdhOffset = Le32(d.data() + kHeaderBytes + static_cast<size_t>(trackIndex) * 4);
    if (tdhOffset == 0)
        return false;

    const size_t tdhBytes = 4 + static_cast<size_t>(hdr.revolutions) * 12;
    if (tdhOffset > d.size() || tdhBytes > d.size() - tdhOffset)
        return false;

    const uint8_t* tdh = d.data() + tdhOffset;
    if (std::memcmp(tdh, "TRK", 3) != 0 || tdh[3] != static_cast<uint8_t>(trackIndex))
        return false;

    out.clear();
    for (int rev = 0; rev < hdr.revolutions; ++rev) {
        const uint8_t* e = tdh + 4 + static_cast<size_t>(rev) * 12;
        // e[0..3] = revolution duration in ticks (unused here - the PLL only
        // needs the intervals), e[4..7] = entry count, e[8..11] = data offset
        // relative to the track data header.
        const uint32_t count = Le32(e + 4);
        const uint32_t rel = Le32(e + 8);

        const size_t offset = static_cast<size_t>(tdhOffset) + rel;
        const size_t bytes = static_cast<size_t>(count) * 2;
        if (count == 0 || offset > d.size() || bytes > d.size() - offset)
            return false;

        out.push_back({offset, count});
    }
    return !out.empty();
}

/**
 * @brief Software data separator: flux intervals in, MFM cell bits out.
 *
 * A direct port of the PLL in scp.c (PLL_authentic mode), which is what both
 * WinUAE and PuAE clock SCP images through. Keeping the same algorithm matters:
 * a track that decodes here is a track the emulator will also read, and a
 * marginal one fails the same way in both.
 */
class FluxPll
{
public:
    FluxPll(const std::vector<uint8_t>& data, uint32_t nsPerTick)
        : m_data(data), m_nsPerTick(nsPerTick)
    {
    }

    /// Clock one revolution's flux entries out as MFM bits, appending to `bits`.
    /// PLL state carries across calls so consecutive revolutions read as one
    /// continuous stream, exactly as a spinning disk would deliver them.
    void ClockRevolution(const RevSpan& span, std::vector<uint8_t>& bits)
    {
        const uint8_t* p = m_data.data() + span.offset;
        uint32_t pending = 0; // accumulated ticks across 0x0000 overflow entries

        for (size_t i = 0; i < span.count; ++i) {
            const uint16_t t = Be16(p + i * 2);
            if (t == 0) {
                pending += 0x10000u; // overflow marker: no reversal in this window
                continue;
            }
            EmitBitsForFlux(static_cast<int>((pending + t) * m_nsPerTick), bits);
            pending = 0;
        }
    }

private:
    // Advance the PLL across one flux interval, pushing the cell bits it clocks
    // out: zeros for the empty windows, then a 1 for the window holding the
    // reversal.
    void EmitBitsForFlux(int fluxNs, std::vector<uint8_t>& bits)
    {
        m_flux += fluxNs;
        m_clockedZeros = 0;

        while (m_flux >= m_clock / 2) {
            m_flux -= m_clock;

            if (m_flux >= m_clock / 2) {
                // Reversal is still beyond this window: an empty cell.
                ++m_clockedZeros;
                Push(bits, 0);
                continue;
            }

            // Adjust the clock by the phase error, then damp back to centre when
            // the run length says we have lost lock (MFM never runs >3 zeros).
            if (m_clockedZeros >= 1 && m_clockedZeros <= 3)
                m_clock += (m_flux / static_cast<int>(m_clockedZeros + 1)) / 10;
            else
                m_clock += (kClockCentreNs - m_clock) / 10;
            m_clock = std::max(kClockMin, std::min(kClockMax, m_clock));

            // Authentic PLL: do not snap the window onto the transition, keep
            // half the residual error so jitter is preserved.
            m_flux /= 2;
            Push(bits, 1);
            return;
        }
    }

    void Push(std::vector<uint8_t>& bits, int bit)
    {
        if ((m_bitCount & 7) == 0)
            bits.push_back(0);
        if (bit)
            bits.back() |= static_cast<uint8_t>(0x80u >> (m_bitCount & 7));
        ++m_bitCount;
    }

    static constexpr int kClockMin = kClockCentreNs * (100 - kClockMaxAdjPercent) / 100;
    static constexpr int kClockMax = kClockCentreNs * (100 + kClockMaxAdjPercent) / 100;

    const std::vector<uint8_t>& m_data;
    uint32_t m_nsPerTick;
    int m_clock = kClockCentreNs;
    int m_flux = 0;
    unsigned m_clockedZeros = 0;
    size_t m_bitCount = 0;
};

} // namespace

bool ScpConverter::IsScp(const std::vector<uint8_t>& data)
{
    return data.size() >= 3 && std::memcmp(data.data(), "SCP", 3) == 0;
}

std::vector<uint8_t> ScpConverter::DecodeTrackMfm(const std::vector<uint8_t>& scpData,
                                                  int trackIndex, int revolutions)
{
    ScpHeader hdr;
    if (!ParseHeader(scpData, hdr))
        return {};

    std::vector<RevSpan> spans;
    if (!ReadTrackSpans(scpData, hdr, trackIndex, spans))
        return {};

    std::vector<uint8_t> bits;
    FluxPll pll(scpData, hdr.nsPerTick);
    // A single-revolution dump still gets clocked twice (the same flux replayed)
    // so a sector straddling the index hole comes out whole on the second pass.
    for (int i = 0; i < std::max(1, revolutions); ++i)
        pll.ClockRevolution(spans[static_cast<size_t>(i) % spans.size()], bits);
    return bits;
}

std::optional<std::vector<uint8_t>>
ScpConverter::ConvertToBuffer(const std::vector<uint8_t>& scpData,
                              std::vector<IpfTrackInfo>* outTracks)
{
    ScpHeader hdr;
    if (!ParseHeader(scpData, hdr)) {
        Logger::Error("SCP: not a SuperCard Pro image or unsupported header");
        return std::nullopt;
    }

    std::vector<uint8_t> adf(static_cast<size_t>(kAdfTracks) * kSectorsPerTrack * kSectorSize, 0);
    int standardTracks = 0;
    int protectedTracks = 0;
    int missingTracks = 0;

    for (int t = hdr.startTrack; t <= hdr.endTrack; ++t) {
        IpfTrackInfo info;
        info.cylinder = static_cast<uint32_t>(t / 2);
        info.head = static_cast<uint32_t>(t % 2);

        // Two revolutions: the second copy carries whatever the index hole cut
        // in half in the first.
        const std::vector<uint8_t> mfm = DecodeTrackMfm(scpData, t, 2);
        if (mfm.empty()) {
            ++missingTracks;
            info.formatName = "Missing";
            if (outTracks)
                outTracks->push_back(info);
            continue;
        }

        // Report one revolution's worth as the physical track length; the
        // decode buffer holds two.
        info.mfmLengthBits = static_cast<uint32_t>(mfm.size() * 8 / 2);

        auto track = AmigaMfmDecoder::DecodeTrack(mfm.data(), mfm.size());
        info.sectorCount = static_cast<uint32_t>(track.sectors.size());
        info.isStandard = track.isStandard;

        if (track.isStandard) {
            info.formatName = "AmigaDOS";
            ++standardTracks;
        } else {
            // Generic labels only, matching the extended-ADF path: identifying
            // the exact scheme would mean running libdisk's trial decoders over
            // every track, which costs more than the Track Map view gains.
            info.isProtection = info.sectorCount == 0;
            info.formatName = info.sectorCount > 0 ? "Partial AmigaDOS" : "Protected / raw MFM";
            if (info.isProtection)
                ++protectedTracks;
        }

        // Place every checksum-valid sector at its logical block, trusting the
        // trackdisk header's own track number as the IPF/ext-ADF paths do.
        for (const auto& sec : track.sectors) {
            if (!sec.checksumOk || sec.sectorNumber >= kSectorsPerTrack ||
                sec.trackNumber >= kAdfTracks)
                continue;
            const size_t block =
                static_cast<size_t>(sec.trackNumber) * kSectorsPerTrack + sec.sectorNumber;
            const size_t offset = block * kSectorSize;
            if (offset + kSectorSize <= adf.size())
                std::copy(sec.data.begin(), sec.data.end(), adf.begin() + offset);
        }

        if (outTracks)
            outTracks->push_back(info);
    }

    Logger::Info("SCP: tracks {}..{} ({} revolutions, {} ns/tick) - {} standard AmigaDOS, {} "
                 "protected/raw, {} unreadable",
                 hdr.startTrack, hdr.endTrack, hdr.revolutions, hdr.nsPerTick, standardTracks,
                 protectedTracks, missingTracks);

    return adf;
}

} // namespace adm::core
