#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/AmigaMfmDecoder.h"
#include "core/ExtAdfConverter.h"

using adm::core::AmigaMfmDecoder;
using adm::core::ExtAdfConverter;
using adm::core::IpfTrackInfo;

namespace {

constexpr int kSecs = AmigaMfmDecoder::kSectorsPerTrack; // 11
constexpr int kSecSize = AmigaMfmDecoder::kSectorSize;   // 512
constexpr size_t kAdfSize = 80u * 2u * kSecs * kSecSize; // 901120

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

void PutBE32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

struct BuiltTrack
{
    uint8_t type;
    std::vector<uint8_t> blob; // stored track data
    uint32_t bitlen;
};

// Assemble a UAE-1ADF image from per-track descriptors + blobs.
std::vector<uint8_t> BuildExtAdf(const std::vector<BuiltTrack>& tracks)
{
    std::vector<uint8_t> img;
    img.insert(img.end(), {'U', 'A', 'E', '-', '1', 'A', 'D', 'F'});
    img.push_back(0);
    img.push_back(0); // reserved
    img.push_back(static_cast<uint8_t>(tracks.size() >> 8));
    img.push_back(static_cast<uint8_t>(tracks.size() & 0xff));

    for (const auto& t : tracks) {
        img.push_back(0);                                   // reserved
        img.push_back(0);                                   // revolutions - 1
        img.push_back(0);                                   // reserved (type high byte)
        img.push_back(t.type);                              // type
        PutBE32(img, static_cast<uint32_t>(t.blob.size())); // len
        PutBE32(img, t.bitlen);                             // bitlen
    }
    for (const auto& t : tracks)
        img.insert(img.end(), t.blob.begin(), t.blob.end());
    return img;
}

// Type 0: decoded sectors stored back-to-back; bitlen = secs*512*8.
BuiltTrack MakeAmigaDosTrack(int track)
{
    BuiltTrack bt;
    bt.type = 0;
    for (const auto& sec : MakeSectors(track))
        bt.blob.insert(bt.blob.end(), sec.begin(), sec.end());
    bt.bitlen = static_cast<uint32_t>(kSecs) * kSecSize * 8;
    return bt;
}

// Type 1: raw MFM bitstream (stamped with the physical track number so it
// self-locates on decode); bitlen = blob bits.
BuiltTrack MakeRawMfmTrack(int track)
{
    BuiltTrack bt;
    bt.type = 1;
    bt.blob = AmigaMfmDecoder::EncodeStandardTrack(static_cast<uint8_t>(track), MakeSectors(track));
    bt.bitlen = static_cast<uint32_t>(bt.blob.size()) * 8;
    return bt;
}

} // namespace

TEST(ExtAdfConverterTest, DetectsMagic)
{
    EXPECT_TRUE(ExtAdfConverter::IsExtAdf({'U', 'A', 'E', '-', '1', 'A', 'D', 'F', 0}));
    EXPECT_FALSE(ExtAdfConverter::IsExtAdf({'D', 'O', 'S', 0}));
    EXPECT_FALSE(ExtAdfConverter::IsExtAdf({})); // empty
}

// A mix of already-decoded AmigaDOS tracks and raw-MFM tracks must produce a
// standard 901120-byte ADF with every sector landing at its logical block.
TEST(ExtAdfConverterTest, DecodesMixedTrackTypes)
{
    std::vector<BuiltTrack> tracks;
    tracks.push_back(MakeAmigaDosTrack(0)); // cyl 0 head 0
    tracks.push_back(MakeRawMfmTrack(1));   // cyl 0 head 1
    tracks.push_back(MakeAmigaDosTrack(2)); // cyl 1 head 0
    tracks.push_back(MakeRawMfmTrack(3));   // cyl 1 head 1

    auto img = BuildExtAdf(tracks);

    std::vector<IpfTrackInfo> info;
    auto adf = ExtAdfConverter::ConvertToBuffer(img, &info);

    ASSERT_TRUE(adf.has_value());
    ASSERT_EQ(adf->size(), kAdfSize);
    ASSERT_EQ(info.size(), tracks.size());

    for (const auto& ti : info) {
        EXPECT_TRUE(ti.isStandard);
        EXPECT_EQ(ti.sectorCount, static_cast<uint32_t>(kSecs));
        EXPECT_EQ(ti.formatName, "AmigaDOS");
    }

    // Every sector of every track must be byte-identical at its logical block.
    for (int track = 0; track < 4; ++track) {
        auto sectors = MakeSectors(track);
        for (int s = 0; s < kSecs; ++s) {
            const size_t off = (static_cast<size_t>(track) * kSecs + s) * kSecSize;
            EXPECT_EQ(0, std::memcmp(adf->data() + off, sectors[s].data(), kSecSize))
                << "track " << track << " sector " << s;
        }
    }
}

// A non-Amiga (unrecoverable) raw track leaves its region blank but is still
// reported, and the volume-mount path can proceed (NDOS).
TEST(ExtAdfConverterTest, NonStandardRawTrackLeftBlank)
{
    std::vector<BuiltTrack> tracks;
    tracks.push_back(MakeAmigaDosTrack(0));

    // A raw track full of a constant byte holds no 0x4489 sync -> no sectors.
    BuiltTrack junk;
    junk.type = 1;
    junk.blob.assign(6000, 0x11);
    junk.bitlen = static_cast<uint32_t>(junk.blob.size()) * 8;
    tracks.push_back(junk);

    auto img = BuildExtAdf(tracks);

    std::vector<IpfTrackInfo> info;
    auto adf = ExtAdfConverter::ConvertToBuffer(img, &info);

    ASSERT_TRUE(adf.has_value());
    ASSERT_EQ(info.size(), 2u);
    EXPECT_TRUE(info[0].isStandard);
    EXPECT_FALSE(info[1].isStandard);
    EXPECT_EQ(info[1].sectorCount, 0u);

    // Track 1's region (blocks 11..21) stays zeroed.
    for (size_t off = static_cast<size_t>(kSecs) * kSecSize;
         off < static_cast<size_t>(2 * kSecs) * kSecSize; ++off)
        ASSERT_EQ((*adf)[off], 0) << "offset " << off;
}

TEST(ExtAdfConverterTest, RejectsMalformedHeader)
{
    // Magic present but track count claims more descriptors than the file holds.
    std::vector<uint8_t> img = {'U', 'A', 'E', '-', '1', 'A', 'D', 'F', 0, 0, 0xFF, 0xFF};
    EXPECT_FALSE(ExtAdfConverter::ConvertToBuffer(img).has_value());
}
