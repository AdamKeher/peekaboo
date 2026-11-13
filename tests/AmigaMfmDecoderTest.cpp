#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "core/AmigaMfmDecoder.h"

using adm::core::AmigaMfmDecoder;

namespace {

// Build 11 distinct 512-byte sector payloads.
std::vector<std::array<uint8_t, AmigaMfmDecoder::kSectorSize>> MakeSectors()
{
    std::vector<std::array<uint8_t, AmigaMfmDecoder::kSectorSize>> sectors(
        AmigaMfmDecoder::kSectorsPerTrack);
    for (int s = 0; s < AmigaMfmDecoder::kSectorsPerTrack; ++s)
        for (int i = 0; i < AmigaMfmDecoder::kSectorSize; ++i)
            sectors[s][i] = static_cast<uint8_t>((s * 37 + i * 11) & 0xff);
    return sectors;
}

} // namespace

// A synthetic standard track must round-trip through encode -> decode with all
// 11 sectors recovered, checksum-valid, and byte-identical to the input.
TEST(AmigaMfmDecoderTest, RoundTripStandardTrack)
{
    const uint8_t trackNumber = 4; // cyl 2, head 0
    auto sectors = MakeSectors();
    auto mfm = AmigaMfmDecoder::EncodeStandardTrack(trackNumber, sectors);

    auto track = AmigaMfmDecoder::DecodeTrack(mfm.data(), mfm.size());

    EXPECT_TRUE(track.isStandard);
    ASSERT_EQ(track.sectors.size(), static_cast<size_t>(AmigaMfmDecoder::kSectorsPerTrack));

    for (const auto& sec : track.sectors) {
        EXPECT_TRUE(sec.checksumOk);
        EXPECT_EQ(sec.trackNumber, trackNumber);
        ASSERT_LT(sec.sectorNumber, AmigaMfmDecoder::kSectorsPerTrack);
        EXPECT_EQ(sec.data, sectors[sec.sectorNumber]);
    }
}

// Sync detection must work even when the track does not start on a byte
// boundary (real tracks are index-aligned, not byte-aligned).
TEST(AmigaMfmDecoderTest, DecodesWithNonByteAlignedGap)
{
    auto sectors = MakeSectors();
    auto mfm = AmigaMfmDecoder::EncodeStandardTrack(7, sectors, /*leadingGapBits=*/5);

    auto track = AmigaMfmDecoder::DecodeTrack(mfm.data(), mfm.size());

    EXPECT_TRUE(track.isStandard);
    EXPECT_EQ(track.sectors.size(), static_cast<size_t>(AmigaMfmDecoder::kSectorsPerTrack));
}

// A corrupted data byte must flip that sector's checksum and make the track
// non-standard, while the other sectors still decode.
TEST(AmigaMfmDecoderTest, CorruptedSectorFailsChecksum)
{
    auto sectors = MakeSectors();
    auto mfm = AmigaMfmDecoder::EncodeStandardTrack(0, sectors);

    // Flip a bit well past the first sync mark, inside sector data.
    mfm[mfm.size() / 2] ^= 0x01;

    auto track = AmigaMfmDecoder::DecodeTrack(mfm.data(), mfm.size());

    EXPECT_FALSE(track.isStandard);
    int badChecksums = 0;
    for (const auto& sec : track.sectors)
        if (!sec.checksumOk)
            ++badChecksums;
    EXPECT_GE(badChecksums, 1);
}

// A flux read spans several revolutions, so a track buffer normally holds more than one copy of
// every sector. When the first copy on the stream is damaged and a later one is intact, the intact
// one must win - recovering that copy is the whole point of reading multiple revolutions.
TEST(AmigaMfmDecoderTest, LaterGoodCopyBeatsEarlierCorruptCopy)
{
    auto sectors = MakeSectors();
    auto one = AmigaMfmDecoder::EncodeStandardTrack(0, sectors);

    // Revolution 1: damaged in sector data. Revolution 2: pristine.
    std::vector<uint8_t> corrupt = one;
    corrupt[corrupt.size() / 2] ^= 0x01;

    // Sanity: on its own, the damaged revolution really does fail a checksum.
    auto damagedOnly = AmigaMfmDecoder::DecodeTrack(corrupt.data(), corrupt.size());
    ASSERT_FALSE(damagedOnly.isStandard);

    std::vector<uint8_t> twoRevs = corrupt;
    twoRevs.insert(twoRevs.end(), one.begin(), one.end());

    auto track = AmigaMfmDecoder::DecodeTrack(twoRevs.data(), twoRevs.size());

    EXPECT_TRUE(track.isStandard);
    ASSERT_EQ(track.sectors.size(), static_cast<size_t>(AmigaMfmDecoder::kSectorsPerTrack));
    for (const auto& sec : track.sectors) {
        EXPECT_TRUE(sec.checksumOk);
        ASSERT_LT(sec.sectorNumber, AmigaMfmDecoder::kSectorsPerTrack);
        EXPECT_EQ(sec.data, sectors[sec.sectorNumber]);
    }
}

// Garbage / too-small input must not crash and must report nothing standard.
TEST(AmigaMfmDecoderTest, HandlesGarbageInput)
{
    std::vector<uint8_t> empty;
    auto t0 = AmigaMfmDecoder::DecodeTrack(empty.data(), empty.size());
    EXPECT_FALSE(t0.isStandard);
    EXPECT_TRUE(t0.sectors.empty());

    std::vector<uint8_t> noise(2000, 0xA5);
    auto t1 = AmigaMfmDecoder::DecodeTrack(noise.data(), noise.size());
    EXPECT_FALSE(t1.isStandard);
}

// --- writable MFM ---------------------------------------------------------
//
// EncodeStandardTrack() is for the decoder's benefit and leaves the clock bits
// zero. EncodeWritableTrack() is for a disk's, and the difference is the whole
// point of these three tests: the first proves the old encoder could never have
// been written to media, the second that the new one can, and the third that
// being writable did not cost the round-trip.

TEST(AmigaMfmDecoderTest, StandardEncodeIsNotWritable)
{
    auto sectors = MakeSectors();
    auto mfm = AmigaMfmDecoder::EncodeStandardTrack(6, sectors);

    size_t badCell = 0;
    EXPECT_FALSE(AmigaMfmDecoder::IsWritableMfm(mfm.data(), mfm.size(), &badCell))
        << "clock bits are zero, so a drive could not clock through this";
}

TEST(AmigaMfmDecoderTest, WritableEncodeObeysTheCellRules)
{
    auto sectors = MakeSectors();
    auto mfm = AmigaMfmDecoder::EncodeWritableTrack(6, sectors);

    // Gap first, then 11 sectors of 1088 bytes.
    EXPECT_EQ(mfm.size(), AmigaMfmDecoder::kDefaultTrackGapBytes +
                              AmigaMfmDecoder::kSectorsPerTrack * AmigaMfmDecoder::kSectorMfmBytes);

    size_t badCell = 0;
    EXPECT_TRUE(AmigaMfmDecoder::IsWritableMfm(mfm.data(), mfm.size(), &badCell))
        << "first offending cell " << badCell;
}

TEST(AmigaMfmDecoderTest, WritableEncodeRoundTrips)
{
    const uint8_t trackNumber = 79; // cyl 39, head 1
    auto sectors = MakeSectors();
    auto mfm = AmigaMfmDecoder::EncodeWritableTrack(trackNumber, sectors);

    auto track = AmigaMfmDecoder::DecodeTrack(mfm.data(), mfm.size());

    EXPECT_TRUE(track.isStandard);
    ASSERT_EQ(track.sectors.size(), static_cast<size_t>(AmigaMfmDecoder::kSectorsPerTrack));
    for (const auto& sec : track.sectors) {
        EXPECT_TRUE(sec.checksumOk) << "sector " << int(sec.sectorNumber);
        EXPECT_EQ(sec.trackNumber, trackNumber);
        ASSERT_LT(sec.sectorNumber, AmigaMfmDecoder::kSectorsPerTrack);
        EXPECT_EQ(sec.data, sectors[sec.sectorNumber]);
    }
}

// The surface-level fields the sector inspector shows: format byte, sectors-to-gap,
// both checksums as stored and as recomputed, and the cell offsets. These are read
// out of the same decode the checksums come from, so the only real risk is the
// arithmetic on the bit positions.
TEST(AmigaMfmDecoderTest, ReportsSurfaceFieldsAndCellOffsets)
{
    const uint8_t trackNumber = 1; // cyl 0, head 1
    auto sectors = MakeSectors();
    auto mfm = AmigaMfmDecoder::EncodeStandardTrack(trackNumber, sectors);

    auto track = AmigaMfmDecoder::DecodeTrack(mfm.data(), mfm.size());
    ASSERT_EQ(track.sectors.size(), static_cast<size_t>(AmigaMfmDecoder::kSectorsPerTrack));

    // One sector body: sync mark, then info + label + both checksums + data, each
    // field stored as an odd half followed by an even half.
    constexpr size_t kSyncBits = 32;
    constexpr size_t kPreDataBits = (4 + 16 + 4 + 4) * 2 * 8; // info, label, hdr ck, data ck
    constexpr size_t kDataBits = AmigaMfmDecoder::kSectorSize * 2 * 8;

    for (const auto& sec : track.sectors) {
        EXPECT_EQ(sec.format, 0xFFu);
        EXPECT_EQ(sec.sectorsToGap,
                  static_cast<uint8_t>(AmigaMfmDecoder::kSectorsPerTrack - sec.sectorNumber));
        EXPECT_TRUE(sec.headerChecksumOk);
        EXPECT_TRUE(sec.dataChecksumOk);
        EXPECT_EQ(sec.headerChecksum, sec.headerChecksumCalc);
        EXPECT_EQ(sec.dataChecksum, sec.dataChecksumCalc);
        // A checksum is the XOR of MFM-encoded longs, so only the data bits survive.
        EXPECT_EQ(sec.headerChecksum & ~0x55555555u, 0u);

        EXPECT_EQ(sec.dataStartCell, sec.syncCell + kSyncBits + kPreDataBits);
        EXPECT_EQ(sec.endCell, sec.dataStartCell + kDataBits);
        EXPECT_EQ(sec.cellCount, sec.endCell - sec.syncCell);
        EXPECT_EQ(sec.cellCount, kSyncBits + kPreDataBits + kDataBits);
    }

    // Sectors are laid down in order. The stride is a whole sector on the surface:
    // the body the offsets above cover, plus the 32-cell 0xAA preamble that precedes
    // every sync mark and is not part of the sector proper.
    constexpr size_t kPreambleBits = 32;
    const auto& first = track.sectors.front();
    for (const auto& sec : track.sectors) {
        const size_t stride = sec.cellCount + kPreambleBits;
        EXPECT_EQ(sec.syncCell, first.syncCell + sec.sectorNumber * stride)
            << "sector " << int(sec.sectorNumber);
    }
}
