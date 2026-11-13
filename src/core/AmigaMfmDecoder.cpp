#include "AmigaMfmDecoder.h"
#include "BigEndian.h"

#include <array>
#include <cstring>
#include <optional>

namespace adm::core {

namespace {

// Amiga trackdisk sync: two 0x4489 words back-to-back.
constexpr uint32_t kSyncMark = 0x44894489u;

// Field sizes (decoded bytes) of a trackdisk sector, in on-disk order.
constexpr size_t kInfoBytes = 4;   // 0xFF, track, sector, sectors-to-gap
constexpr size_t kLabelBytes = 16; // OS recovery label
constexpr size_t kHdrCkBytes = 4;  // header checksum (1 long)
constexpr size_t kDataCkBytes = 4; // data checksum (1 long)
constexpr size_t kDataBytes = AmigaMfmDecoder::kSectorSize;

// Total MFM-encoded bits consumed by one sector body after the sync mark
// (each field is stored as an odd half followed by an even half => x2 bytes).
constexpr size_t kSectorBodyBits =
    (kInfoBytes + kLabelBytes + kHdrCkBytes + kDataCkBytes + kDataBytes) * 2 * 8;

// XOR of big-endian longs over an MFM-encoded region, masked to the data bits.
// This is the trackdisk header/data checksum algorithm.
uint32_t MfmChecksum(const uint8_t* enc, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t v = (static_cast<uint32_t>(enc[i]) << 24) |
                     (static_cast<uint32_t>(enc[i + 1]) << 16) |
                     (static_cast<uint32_t>(enc[i + 2]) << 8) | static_cast<uint32_t>(enc[i + 3]);
        sum ^= v;
    }
    return sum & 0x55555555u;
}

uint32_t Be32(const uint8_t* p) { return be::U32(p); }

// Sequential MSB-first bit reader over an MFM track buffer.
class BitReader
{
public:
    BitReader(const uint8_t* buf, size_t lenBytes) : m_buf(buf), m_totalBits(lenBytes * 8) {}

    size_t totalBits() const { return m_totalBits; }

    uint32_t bitAt(size_t pos) const { return (m_buf[pos >> 3] >> (7 - (pos & 7))) & 1u; }

    // Read one MFM field of `nData` decoded bytes starting at bit `pos`.
    // Fills `dec` (nData bytes) and `enc` (2*nData encoded bytes for checksums),
    // advancing `pos`. Returns false if the field runs past the buffer end.
    bool readField(size_t& pos, size_t nData, uint8_t* dec, uint8_t* enc) const
    {
        const size_t encLen = nData * 2;
        if (pos + encLen * 8 > m_totalBits)
            return false;
        for (size_t i = 0; i < encLen; ++i) {
            uint8_t b = 0;
            for (int k = 0; k < 8; ++k)
                b = static_cast<uint8_t>((b << 1) | bitAt(pos++));
            enc[i] = b;
        }
        for (size_t i = 0; i < nData; ++i)
            dec[i] = static_cast<uint8_t>(((enc[i] & 0x55) << 1) | (enc[nData + i] & 0x55));
        return true;
    }

private:
    const uint8_t* m_buf;
    size_t m_totalBits;
};

// Bit-level MFM writer used only by EncodeStandardTrack (tests / synthetic data).
class BitWriter
{
public:
    void pushBit(uint32_t b)
    {
        m_cur = static_cast<uint8_t>((m_cur << 1) | (b & 1u));
        if (++m_nbits == 8) {
            m_out.push_back(m_cur);
            m_cur = 0;
            m_nbits = 0;
        }
    }
    void pushByte(uint8_t v)
    {
        for (int k = 7; k >= 0; --k)
            pushBit((v >> k) & 1u);
    }
    void pushWord(uint16_t w)
    {
        pushByte(static_cast<uint8_t>(w >> 8));
        pushByte(static_cast<uint8_t>(w & 0xff));
    }
    // Encode `nData` bytes as an odd-half/even-half MFM field (clock bits = 0).
    // Appends to the stream and to `enc` (for checksum computation).
    void pushField(const uint8_t* data, size_t nData, std::vector<uint8_t>& enc)
    {
        for (size_t i = 0; i < nData; ++i) {
            uint8_t odd = static_cast<uint8_t>((data[i] >> 1) & 0x55);
            enc.push_back(odd);
            pushByte(odd);
        }
        for (size_t i = 0; i < nData; ++i) {
            uint8_t even = static_cast<uint8_t>(data[i] & 0x55);
            enc.push_back(even);
            pushByte(even);
        }
    }
    std::vector<uint8_t> finish()
    {
        if (m_nbits > 0) {
            m_cur = static_cast<uint8_t>(m_cur << (8 - m_nbits));
            m_out.push_back(m_cur);
            m_cur = 0;
            m_nbits = 0;
        }
        return std::move(m_out);
    }

private:
    std::vector<uint8_t> m_out;
    uint8_t m_cur = 0;
    int m_nbits = 0;
};

} // namespace

AmigaMfmDecoder::Track AmigaMfmDecoder::DecodeTrack(const uint8_t* mfm, size_t mfmLenBytes)
{
    Track track;
    if (!mfm || mfmLenBytes < 4)
        return track;

    BitReader reader(mfm, mfmLenBytes);
    const size_t total = reader.totalBits();

    // First checksum-valid copy of each sector number wins.
    std::array<std::optional<Sector>, kSectorsPerTrack> found{};

    // Slide a 32-bit window over the bitstream; every time it equals the two
    // 0x4489 sync words, attempt to decode the sector body that follows.
    uint32_t window = 0;
    std::vector<uint8_t> dataEnc(kDataBytes * 2);
    for (size_t pos = 0; pos < total; ++pos) {
        window = (window << 1) | reader.bitAt(pos);
        if (window != kSyncMark)
            continue;

        size_t bit = pos + 1;
        if (bit + kSectorBodyBits > total)
            continue;

        uint8_t info[kInfoBytes], infoEnc[kInfoBytes * 2];
        uint8_t label[kLabelBytes], labelEnc[kLabelBytes * 2];
        uint8_t hck[kHdrCkBytes], hckEnc[kHdrCkBytes * 2];
        uint8_t dck[kDataCkBytes], dckEnc[kDataCkBytes * 2];

        Sector sec;
        // pos sits on the last bit of the 32-bit sync window, so the mark itself
        // starts 31 bits back.
        sec.syncCell = pos - 31;
        if (!reader.readField(bit, kInfoBytes, info, infoEnc) ||
            !reader.readField(bit, kLabelBytes, label, labelEnc) ||
            !reader.readField(bit, kHdrCkBytes, hck, hckEnc) ||
            !reader.readField(bit, kDataCkBytes, dck, dckEnc))
            continue;
        sec.dataStartCell = bit;
        if (!reader.readField(bit, kDataBytes, sec.data.data(), dataEnc.data()))
            continue;
        sec.endCell = bit;
        sec.cellCount = sec.endCell - sec.syncCell;

        sec.format = info[0];
        sec.trackNumber = info[1];
        sec.sectorNumber = info[2];
        sec.sectorsToGap = info[3];
        std::memcpy(sec.label.data(), label, kLabelBytes);

        // Header checksum spans the info + label encoded longs.
        uint8_t headerEnc[(kInfoBytes + kLabelBytes) * 2];
        for (size_t i = 0; i < sizeof(infoEnc); ++i)
            headerEnc[i] = infoEnc[i];
        for (size_t i = 0; i < sizeof(labelEnc); ++i)
            headerEnc[sizeof(infoEnc) + i] = labelEnc[i];

        const uint32_t hdrCalc = MfmChecksum(headerEnc, sizeof(headerEnc));
        const uint32_t dataCalc = MfmChecksum(dataEnc.data(), dataEnc.size());
        sec.headerChecksum = Be32(hck);
        sec.headerChecksumCalc = hdrCalc;
        sec.dataChecksum = Be32(dck);
        sec.dataChecksumCalc = dataCalc;
        sec.headerChecksumOk = (hdrCalc == sec.headerChecksum);
        sec.dataChecksumOk = (dataCalc == sec.dataChecksum);
        sec.checksumOk = sec.headerChecksumOk && sec.dataChecksumOk;

        if (sec.sectorNumber >= kSectorsPerTrack)
            continue;
        // A flux read spans several revolutions, so the same sector normally shows up more than
        // once. Take the first copy that passes both checksums; only fall back to a failing copy
        // when nothing better has been seen, and let a later good copy replace it.
        auto& slot = found[sec.sectorNumber];
        if (!slot.has_value() || (sec.checksumOk && !slot->checksumOk))
            slot = sec;
    }

    int validCount = 0;
    for (auto& s : found) {
        if (s.has_value()) {
            track.sectors.push_back(*s);
            if (s->checksumOk)
                ++validCount;
        }
    }
    track.isStandard = (validCount == kSectorsPerTrack);
    return track;
}

namespace {

void PutBe32(uint32_t v, uint8_t* d)
{
    d[0] = static_cast<uint8_t>(v >> 24);
    d[1] = static_cast<uint8_t>(v >> 16);
    d[2] = static_cast<uint8_t>(v >> 8);
    d[3] = static_cast<uint8_t>(v);
}

// The two halves a long is split into on the surface: the odd-numbered data bits
// first, then the even ones. Clock positions are left clear for FillClockBits().
uint32_t OddHalf(uint32_t v) { return (v >> 1) & 0x55555555u; }
uint32_t EvenHalf(uint32_t v) { return v & 0x55555555u; }

constexpr uint32_t kSyncLong = 0x44894489u;

// Every byte on the surface is clock,data,clock,data... MSB first, so odd cell
// indices carry data and even ones carry clock.
bool CellAt(const std::vector<uint8_t>& s, long long k)
{
    if (k < 0 || static_cast<size_t>(k) >= s.size() * 8)
        return false;
    return ((s[static_cast<size_t>(k) >> 3] >> (7 - (static_cast<size_t>(k) & 7))) & 1u) != 0;
}

void SetCell(std::vector<uint8_t>& s, size_t k, bool on)
{
    const uint8_t mask = static_cast<uint8_t>(1u << (7 - (k & 7)));
    if (on)
        s[k >> 3] |= mask;
    else
        s[k >> 3] = static_cast<uint8_t>(s[k >> 3] & ~mask);
}

/*
   Fill every clock cell in one pass over the whole stream.

   A clock cell is set exactly when the data cells either side of it are both zero.
   Doing it in one pass over the finished stream, rather than field by field, is what
   makes the rule hold across field boundaries as well as inside them - the device's
   own encoder patches the boundaries it knows about and leaves three per sector
   slightly wrong, which a PLL absorbs but which there is no reason to reproduce.

   Data cells are only read, never written, so this is safe in place. `isSync` marks
   the bytes to leave alone: a sync mark breaks the rules deliberately.
*/
void FillClockBits(std::vector<uint8_t>& s, const std::function<bool(size_t)>& isSync)
{
    for (size_t i = 0; i < s.size(); ++i) {
        if (isSync(i))
            continue;
        for (size_t c = 0; c < 8; c += 2) {
            const size_t k = i * 8 + c;
            SetCell(s, k, !CellAt(s, static_cast<long long>(k) - 1) && !CellAt(s, k + 1));
        }
    }
}

} // namespace

std::vector<uint8_t> AmigaMfmDecoder::EncodeWritableTrack(
    uint8_t trackNumber, const std::vector<std::array<uint8_t, kSectorSize>>& sectorData,
    size_t trackGapBytes)
{
    const size_t count = sectorData.size();
    // 0xAA is a run of zero data cells with every clock cell set - the gap, and the
    // right starting point for every field whose data bits are zero.
    std::vector<uint8_t> out(trackGapBytes + count * kSectorMfmBytes, 0xAA);

    for (size_t s = 0; s < count; ++s) {
        uint8_t* d = out.data() + trackGapBytes + s * kSectorMfmBytes;

        PutBe32(kSyncLong, d + 4);

        const uint32_t info = 0xFF000000u | (static_cast<uint32_t>(trackNumber) << 16) |
                              (static_cast<uint32_t>(s) << 8) | static_cast<uint32_t>(count - s);
        PutBe32(OddHalf(info), d + 8);
        PutBe32(EvenHalf(info), d + 12);

        // Sector label: zero, as AmigaDOS writes it. Data cells clear, clock cells
        // filled below - which is the 0xAA it already holds.
        std::memset(d + 16, 0, kLabelBytes * 2);

        for (size_t i = 0; i < kSectorSize; ++i) {
            d[64 + i] = static_cast<uint8_t>((sectorData[s][i] >> 1) & 0x55);
            d[64 + kSectorSize + i] = static_cast<uint8_t>(sectorData[s][i] & 0x55);
        }

        // Both checksums are over the ENCODED longs - the halves as they sit on the
        // surface, clock cells still clear, which is exactly what MfmChecksum wants
        // and what the decoder recomputes.
        const uint32_t hdrCk = MfmChecksum(d + 8, (kInfoBytes + kLabelBytes) * 2);
        PutBe32(OddHalf(hdrCk), d + 48);
        PutBe32(EvenHalf(hdrCk), d + 52);

        const uint32_t dataCk = MfmChecksum(d + 64, kSectorSize * 2);
        PutBe32(OddHalf(dataCk), d + 56);
        PutBe32(EvenHalf(dataCk), d + 60);
    }

    const size_t gap = trackGapBytes;
    FillClockBits(out, [gap, count](size_t i) {
        if (i < gap)
            return false;
        const size_t off = (i - gap) % kSectorMfmBytes;
        return (i - gap) / kSectorMfmBytes < count && off >= 4 && off < 8;
    });

    return out;
}

bool AmigaMfmDecoder::IsWritableMfm(const uint8_t* mfm, size_t lenBytes, size_t* firstBadCell)
{
    if (!mfm || lenBytes == 0)
        return false;

    const std::vector<uint8_t> s(mfm, mfm + lenBytes);
    const size_t cells = lenBytes * 8;

    // Two adjacent transitions are impossible for the write circuitry to place, and
    // the sync mark does not contain a pair - so this rule has no exception.
    for (size_t k = 1; k < cells; ++k) {
        if (CellAt(s, k) && CellAt(s, k - 1)) {
            if (firstBadCell)
                *firstBadCell = k;
            return false;
        }
    }

    // A run of four zero cells is legal only as the one inside a sync mark. Find the
    // syncs first, by their pattern, so this works on any stream and not only on one
    // this class produced.
    std::vector<bool> inSync(cells, false);
    uint32_t window = 0;
    for (size_t k = 0; k < cells; ++k) {
        window = (window << 1) | (CellAt(s, k) ? 1u : 0u);
        if (k >= 31 && window == kSyncLong) {
            for (size_t j = k - 31; j <= k; ++j)
                inSync[j] = true;
        }
    }

    size_t run = 0;
    for (size_t k = 0; k < cells; ++k) {
        if (CellAt(s, k)) {
            run = 0;
            continue;
        }
        ++run;
        if (run > 3 && !inSync[k]) {
            if (firstBadCell)
                *firstBadCell = k;
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> AmigaMfmDecoder::EncodeStandardTrack(
    uint8_t trackNumber, const std::vector<std::array<uint8_t, kSectorSize>>& sectorData,
    uint32_t leadingGapBits)
{
    BitWriter w;
    for (uint32_t i = 0; i < leadingGapBits; ++i)
        w.pushBit(0);

    const size_t count = sectorData.size();
    for (size_t s = 0; s < count; ++s) {
        // Gap / preamble of MFM zeros (cannot contain the sync pattern).
        w.pushWord(0x0000);
        w.pushWord(0x0000);
        // Two sync words.
        w.pushWord(0x4489);
        w.pushWord(0x4489);

        uint8_t info[kInfoBytes] = {0xFF, trackNumber, static_cast<uint8_t>(s),
                                    static_cast<uint8_t>(kSectorsPerTrack - s)};
        uint8_t label[kLabelBytes] = {0};

        std::vector<uint8_t> infoEnc, labelEnc, dataEnc;
        w.pushField(info, kInfoBytes, infoEnc);
        w.pushField(label, kLabelBytes, labelEnc);

        // Header checksum over info + label encoded longs.
        std::vector<uint8_t> headerEnc = infoEnc;
        headerEnc.insert(headerEnc.end(), labelEnc.begin(), labelEnc.end());
        uint32_t hdrCk = MfmChecksum(headerEnc.data(), headerEnc.size());

        // Pre-compute the data checksum by encoding the data into a scratch buffer.
        for (size_t i = 0; i < kDataBytes; ++i) {
            dataEnc.push_back(static_cast<uint8_t>((sectorData[s][i] >> 1) & 0x55));
        }
        for (size_t i = 0; i < kDataBytes; ++i) {
            dataEnc.push_back(static_cast<uint8_t>(sectorData[s][i] & 0x55));
        }
        uint32_t dataCk = MfmChecksum(dataEnc.data(), dataEnc.size());

        uint8_t hck[kHdrCkBytes] = {static_cast<uint8_t>(hdrCk >> 24),
                                    static_cast<uint8_t>(hdrCk >> 16),
                                    static_cast<uint8_t>(hdrCk >> 8), static_cast<uint8_t>(hdrCk)};
        uint8_t dck[kDataCkBytes] = {
            static_cast<uint8_t>(dataCk >> 24), static_cast<uint8_t>(dataCk >> 16),
            static_cast<uint8_t>(dataCk >> 8), static_cast<uint8_t>(dataCk)};

        std::vector<uint8_t> scratch;
        w.pushField(hck, kHdrCkBytes, scratch);
        scratch.clear();
        w.pushField(dck, kDataCkBytes, scratch);
        scratch.clear();
        w.pushField(sectorData[s].data(), kDataBytes, scratch);
    }
    // Trailing gap so the last sector body is fully inside the buffer.
    w.pushWord(0x0000);
    w.pushWord(0x0000);
    return w.finish();
}

} // namespace adm::core
