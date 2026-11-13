#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/AmigaMfmDecoder.h"
#include "core/ScpConverter.h"

using adm::core::AmigaMfmDecoder;
using adm::core::IpfTrackInfo;
using adm::core::ScpConverter;

namespace {

constexpr int kSecs = AmigaMfmDecoder::kSectorsPerTrack; // 11
constexpr int kSecSize = AmigaMfmDecoder::kSectorSize;   // 512
constexpr size_t kAdfSize = 80u * 2u * kSecs * kSecSize; // 901120

constexpr size_t kHeaderBytes = 0x10;
constexpr size_t kTrackTableBytes = 168 * 4;

// A nominal DD bit cell is 2 us and the SuperCard Pro samples at 25 ns, so a
// perfectly-written cell is exactly 80 flux ticks wide.
constexpr uint32_t kTicksPerCell = 2000 / 25;

using SectorPayload = std::array<uint8_t, kSecSize>;

// Deterministic, track-distinct sector payloads so we can assert placement.
std::vector<SectorPayload> MakeSectors(int track)
{
    std::vector<SectorPayload> sectors(kSecs);
    for (int s = 0; s < kSecs; ++s)
        for (int i = 0; i < kSecSize; ++i)
            sectors[s][i] = static_cast<uint8_t>((track * 101 + s * 37 + i * 11) & 0xff);
    return sectors;
}

void PutLE32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

// Turn an MFM cell bitstream into the flux intervals a drive would produce
// reading it back: one 16-bit big-endian tick count per reversal (per set cell).
std::vector<uint8_t> MfmToFlux(const std::vector<uint8_t>& mfm)
{
    std::vector<uint8_t> flux;
    size_t prev = 0;
    const size_t totalBits = mfm.size() * 8;
    for (size_t bit = 0; bit < totalBits; ++bit) {
        if (!((mfm[bit >> 3] >> (7 - (bit & 7))) & 1u))
            continue;
        const uint32_t ticks = static_cast<uint32_t>(bit - prev + 1) * kTicksPerCell;
        flux.push_back(static_cast<uint8_t>(ticks >> 8));
        flux.push_back(static_cast<uint8_t>(ticks & 0xff));
        prev = bit + 1;
    }
    // The encoder's trailing gap holds no reversal, so without this the last
    // cells - and with them the tail of the final sector - would simply vanish
    // from the flux. A real dump always has the next revolution's first reversal
    // there; stand one in for it.
    if (prev < totalBits) {
        const uint32_t ticks = static_cast<uint32_t>(totalBits - prev + 1) * kTicksPerCell;
        flux.push_back(static_cast<uint8_t>(ticks >> 8));
        flux.push_back(static_cast<uint8_t>(ticks & 0xff));
    }
    return flux;
}

struct BuiltTrack
{
    int index = 0;             // SCP track number (cyl * 2 + head)
    std::vector<uint8_t> flux; // big-endian 16-bit tick counts
    bool present = true;       // false = leave the offset-table entry zero
};

// Assemble a single-revolution SCP image from per-track flux blobs.
std::vector<uint8_t> BuildScp(const std::vector<BuiltTrack>& tracks, uint8_t startTrack,
                              uint8_t endTrack, uint8_t revolutions = 1, uint8_t bitCellWidth = 0)
{
    std::vector<uint8_t> img(kHeaderBytes + kTrackTableBytes, 0);
    img[0] = 'S';
    img[1] = 'C';
    img[2] = 'P';
    img[3] = 0x18;         // version 1.8
    img[4] = 0x80;         // Commodore Amiga DD
    img[5] = revolutions;  //
    img[6] = startTrack;   //
    img[7] = endTrack;     //
    img[8] = 0x01;         // flags: INDEX
    img[9] = bitCellWidth; // 0 = 16-bit flux entries
    img[10] = 0;           // both heads
    img[11] = 0;           // 25 ns resolution

    for (const auto& t : tracks) {
        if (!t.present)
            continue;

        const uint32_t tdhOffset = static_cast<uint32_t>(img.size());
        const size_t tableSlot = kHeaderBytes + static_cast<size_t>(t.index) * 4;
        img[tableSlot + 0] = static_cast<uint8_t>(tdhOffset);
        img[tableSlot + 1] = static_cast<uint8_t>(tdhOffset >> 8);
        img[tableSlot + 2] = static_cast<uint8_t>(tdhOffset >> 16);
        img[tableSlot + 3] = static_cast<uint8_t>(tdhOffset >> 24);

        img.insert(img.end(), {'T', 'R', 'K'});
        img.push_back(static_cast<uint8_t>(t.index));

        const uint32_t entries = static_cast<uint32_t>(t.flux.size() / 2);
        const uint32_t dataRel = 4 + revolutions * 12u;
        for (uint8_t rev = 0; rev < revolutions; ++rev) {
            PutLE32(img, entries * kTicksPerCell); // revolution duration (unused)
            PutLE32(img, entries);                 // flux entry count
            PutLE32(img, dataRel);                 // every revolution shares one blob
        }
        img.insert(img.end(), t.flux.begin(), t.flux.end());
    }
    return img;
}

// A standard AmigaDOS track, encoded and then "read back" as flux.
BuiltTrack StandardTrack(int index)
{
    const auto mfm =
        AmigaMfmDecoder::EncodeStandardTrack(static_cast<uint8_t>(index), MakeSectors(index));
    return {index, MfmToFlux(mfm), true};
}

const IpfTrackInfo& TrackFor(const std::vector<IpfTrackInfo>& tracks, uint32_t cyl, uint32_t head)
{
    for (const auto& t : tracks)
        if (t.cylinder == cyl && t.head == head)
            return t;
    ADD_FAILURE() << "no metadata for cylinder " << cyl << " head " << head;
    static IpfTrackInfo empty;
    return empty;
}

} // namespace

TEST(ScpConverterTest, DetectsMagic)
{
    EXPECT_TRUE(ScpConverter::IsScp({'S', 'C', 'P', 0x18}));
    EXPECT_FALSE(ScpConverter::IsScp({'C', 'A', 'P', 'S'}));
    EXPECT_FALSE(ScpConverter::IsScp({'S', 'C'}));
    EXPECT_FALSE(ScpConverter::IsScp({}));
}

TEST(ScpConverterTest, DecodesStandardTracksFromFlux)
{
    const std::vector<BuiltTrack> tracks = {StandardTrack(0), StandardTrack(1), StandardTrack(2),
                                            StandardTrack(3)};
    const auto img = BuildScp(tracks, 0, 3);

    std::vector<IpfTrackInfo> info;
    auto adf = ScpConverter::ConvertToBuffer(img, &info);
    ASSERT_TRUE(adf.has_value());
    ASSERT_EQ(adf->size(), kAdfSize);
    ASSERT_EQ(info.size(), 4u);

    for (int t = 0; t <= 3; ++t) {
        const auto& ti = TrackFor(info, static_cast<uint32_t>(t / 2), static_cast<uint32_t>(t % 2));
        EXPECT_TRUE(ti.isStandard) << "track " << t;
        EXPECT_EQ(ti.sectorCount, static_cast<uint32_t>(kSecs)) << "track " << t;
        EXPECT_EQ(ti.formatName, "AmigaDOS") << "track " << t;
        EXPECT_FALSE(ti.isProtection) << "track " << t;

        const auto expected = MakeSectors(t);
        for (int s = 0; s < kSecs; ++s) {
            const size_t offset = (static_cast<size_t>(t) * kSecs + s) * kSecSize;
            EXPECT_EQ(std::memcmp(adf->data() + offset, expected[s].data(), kSecSize), 0)
                << "track " << t << " sector " << s;
        }
    }
}

TEST(ScpConverterTest, ReportsPhysicalTrackLength)
{
    const auto img = BuildScp({StandardTrack(0)}, 0, 0);

    std::vector<IpfTrackInfo> info;
    ASSERT_TRUE(ScpConverter::ConvertToBuffer(img, &info).has_value());
    ASSERT_EQ(info.size(), 1u);

    // The synthetic track has no gap padding beyond the encoder's, so it is far
    // shorter than a real 100 kbit revolution - but it must still report roughly
    // the encoded cell count, not the two-revolution decode buffer.
    const auto mfm = AmigaMfmDecoder::EncodeStandardTrack(0, MakeSectors(0));
    const uint32_t encodedBits = static_cast<uint32_t>(mfm.size() * 8);
    EXPECT_GT(info[0].mfmLengthBits, encodedBits - 64);
    EXPECT_LT(info[0].mfmLengthBits, encodedBits + 64);
}

TEST(ScpConverterTest, MarksUndecodableTracksAsProtected)
{
    // Flux with no recoverable sync pattern: an unbroken run of nominal cells.
    std::vector<uint8_t> flux;
    for (int i = 0; i < 4000; ++i) {
        flux.push_back(static_cast<uint8_t>((kTicksPerCell * 2) >> 8));
        flux.push_back(static_cast<uint8_t>((kTicksPerCell * 2) & 0xff));
    }

    const auto img = BuildScp({StandardTrack(0), BuiltTrack{1, flux, true}}, 0, 1);

    std::vector<IpfTrackInfo> info;
    auto adf = ScpConverter::ConvertToBuffer(img, &info);
    ASSERT_TRUE(adf.has_value());
    ASSERT_EQ(info.size(), 2u);

    EXPECT_TRUE(info[0].isStandard);
    EXPECT_FALSE(info[1].isStandard);
    EXPECT_TRUE(info[1].isProtection);
    EXPECT_EQ(info[1].sectorCount, 0u);
    EXPECT_EQ(info[1].formatName, "Protected / raw MFM");

    // A track that decoded to nothing leaves its ADF region untouched.
    for (size_t i = kSecs * kSecSize; i < 2u * kSecs * kSecSize; ++i)
        ASSERT_EQ((*adf)[i], 0u) << "byte " << i;
}

TEST(ScpConverterTest, ReportsAbsentTracksAsMissing)
{
    const auto img = BuildScp({StandardTrack(0), BuiltTrack{1, {}, false}}, 0, 1);

    std::vector<IpfTrackInfo> info;
    ASSERT_TRUE(ScpConverter::ConvertToBuffer(img, &info).has_value());
    ASSERT_EQ(info.size(), 2u);
    EXPECT_EQ(info[1].formatName, "Missing");
    EXPECT_EQ(info[1].sectorCount, 0u);
    EXPECT_EQ(info[1].mfmLengthBits, 0u);
}

TEST(ScpConverterTest, MultiRevolutionImagesDecode)
{
    // Three stored revolutions of the same flux: the converter should clock two
    // of them and still land the sectors exactly once each.
    const auto track = StandardTrack(0);
    const auto img = BuildScp({track}, 0, 0, /*revolutions=*/3);

    std::vector<IpfTrackInfo> info;
    auto adf = ScpConverter::ConvertToBuffer(img, &info);
    ASSERT_TRUE(adf.has_value());
    ASSERT_EQ(info.size(), 1u);
    EXPECT_TRUE(info[0].isStandard);

    const auto expected = MakeSectors(0);
    for (int s = 0; s < kSecs; ++s)
        EXPECT_EQ(std::memcmp(adf->data() + static_cast<size_t>(s) * kSecSize, expected[s].data(),
                              kSecSize),
                  0)
            << "sector " << s;
}

TEST(ScpConverterTest, RejectsMalformedHeaders)
{
    // Zero revolutions.
    EXPECT_FALSE(ScpConverter::ConvertToBuffer(BuildScp({StandardTrack(0)}, 0, 0, 0)).has_value());

    // Unsupported flux entry width.
    EXPECT_FALSE(
        ScpConverter::ConvertToBuffer(BuildScp({StandardTrack(0)}, 0, 0, 1, 8)).has_value());

    // Truncated before the end of the track offset table.
    auto truncated = BuildScp({StandardTrack(0)}, 0, 0);
    truncated.resize(kHeaderBytes + 16);
    EXPECT_FALSE(ScpConverter::ConvertToBuffer(truncated).has_value());

    // Right size, wrong magic.
    auto notScp = BuildScp({StandardTrack(0)}, 0, 0);
    notScp[0] = 'X';
    EXPECT_FALSE(ScpConverter::ConvertToBuffer(notScp).has_value());
}

TEST(ScpConverterTest, DecodeTrackMfmRecoversTheEncodedStream)
{
    const auto mfm = AmigaMfmDecoder::EncodeStandardTrack(7, MakeSectors(7));
    const auto img = BuildScp({{7, MfmToFlux(mfm), true}}, 7, 7);

    const auto oneRev = ScpConverter::DecodeTrackMfm(img, 7, 1);
    ASSERT_FALSE(oneRev.empty());
    const auto twoRevs = ScpConverter::DecodeTrackMfm(img, 7, 2);
    EXPECT_NEAR(static_cast<double>(twoRevs.size()), 2.0 * static_cast<double>(oneRev.size()), 2.0);

    // The recovered stream must contain the sectors we encoded.
    auto decoded = AmigaMfmDecoder::DecodeTrack(oneRev.data(), oneRev.size());
    EXPECT_TRUE(decoded.isStandard);

    // A track that is not in the image yields nothing rather than garbage.
    EXPECT_TRUE(ScpConverter::DecodeTrackMfm(img, 8, 2).empty());
    EXPECT_TRUE(ScpConverter::DecodeTrackMfm(img, -1, 2).empty());
    EXPECT_TRUE(ScpConverter::DecodeTrackMfm(img, 999, 2).empty());
}
